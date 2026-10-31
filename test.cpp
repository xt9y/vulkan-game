#include "clt/input/input.hpp"
#include "clt/network/connection.hpp"
#include "clt/platform/window.hpp"
#include "clt/player/prediction.hpp"
#include "clt/render/renderer.hpp"
#include "clt/view/camera.hpp"
#include "srv/app/application.hpp"
#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/shared/net/scene_stream.hpp"
#include "srv/shared/net/socket.hpp"
#include "srv/shared/player/movement.hpp"
#include "srv/world/worker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    constexpr std::uint16_t server_port = 37981;
    constexpr std::uint16_t proxy_port = 37982;

    struct proxy_state
    {
        std::atomic<bool> stop{false};
        std::atomic<int> loss{0};
        std::atomic<int> delay_ms{0};
        std::atomic<std::uint64_t> delivered{0};
        std::atomic<std::uint64_t> dropped{0};
    };

    struct datagram
    {
        std::vector<std::uint8_t> bytes;
        net::endpoint::resource destination;
        clock::time_point due;
        bool upstream = false;
    };

    // Relays the game's actual UDP traffic. Delay, jitter and loss affect both
    // directions, including scene transfer, heartbeats, inputs and snapshots.
    void proxy_run(proxy_state& state)
    {
        auto down = net::socket::create(proxy_port, "127.0.0.1");
        auto up = net::socket::create();
        const auto server = net::endpoint::resolve("127.0.0.1", server_port);
        net::endpoint::resource client{};
        bool have_client = false;

        std::mt19937 random(87121);
        std::uniform_int_distribution<int> percentage(0, 99);
        std::vector<datagram> pending;
        pending.reserve(4096);

        std::array<std::uint8_t, 2048> bytes{};

        while (!state.stop.load(std::memory_order_relaxed))
        {
            for (int side = 0; side < 2; ++side)
            {
                auto& socket = side ? up : down;

                for (int i = 0; i < 128; ++i)
                {
                    net::endpoint::resource sender;
                    const auto size = net::socket::receive(
                        socket, sender, bytes.data(), bytes.size()
                    );

                    if (!size)
                    {
                        break;
                    }

                    if (!side)
                    {
                        client = sender;
                        have_client = true;
                    }
                    else if (!have_client ||
                             !net::endpoint::equal(sender, server))
                    {
                        continue;
                    }

                    if (percentage(random) <
                        state.loss.load(std::memory_order_relaxed))
                    {
                        state.dropped.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }

                    const int latency = state.delay_ms.load(std::memory_order_relaxed);
                    const int jitter = latency > 0
                        ? std::uniform_int_distribution<int>(0, latency * 2)(random)
                        : 0;

                    datagram packet;
                    packet.bytes.assign(bytes.begin(), bytes.begin() + *size);
                    packet.destination = side ? client : server;
                    packet.upstream = !side;
                    packet.due = clock::now() + std::chrono::milliseconds(jitter);

                    pending.push_back(std::move(packet));
                }
            }

            const auto now = clock::now();

            for (std::size_t i = 0; i < pending.size();)
            {
                if (pending[i].due > now)
                {
                    ++i;
                    continue;
                }

                auto& socket = pending[i].upstream ? up : down;
                net::socket::send(
                    socket, pending[i].destination,
                    pending[i].bytes.data(), pending[i].bytes.size()
                );

                state.delivered.fetch_add(1, std::memory_order_relaxed);
                pending[i] = std::move(pending.back());
                pending.pop_back();
            }

            net::socket::wait_readable(down, 1ms);
        }

        net::socket::destroy(up);
        net::socket::destroy(down);
    }

    std::size_t instance_count()
    {
        const char* value = std::getenv("WORKLOAD_INSTANCES");

        if (!value)
        {
            return 12000;
        }

        const auto parsed = std::stoul(value);

        if (parsed < 1 || parsed > 100000)
        {
            throw std::invalid_argument("WORKLOAD_INSTANCES must be 1..100000");
        }

        return parsed;
    }

    void build_stress_scene(std::size_t count)
    {
        auto& server = *srv::application::get();

        // Reuse the existing cube mesh. This stresses the real indexed drawing
        // path, transforms, depth, scene transfer and collision cache.
        for (std::size_t i = 0; i < count; ++i)
        {
            ::scene::instance cube;
            cube.id = static_cast<std::uint32_t>(100 + i);
            cube.mesh_id = 2;

            const float x = float(int(i % 200) - 100) * 1.6f;
            const float z = -float(i / 200) * 1.6f - 8.0f;
            cube.pose.position = {x, 0.5f, z};

            server.scene.instances.push_back(cube);
        }

        server.colliders = ::player::build_colliders(server.scene);
        server.scene_bytes = net::scene_stream::serialize(server.scene);
        server.scene_hash = net::scene_stream::checksum(server.scene_bytes);
    }

    struct result
    {
        std::string name;
        std::vector<double> frame_ms;
        double seconds = 0;
        double simulation_hz = 0;
        double server_hz = 0;
        double snapshots_hz = 0;
        double max_correction = 0;
        std::uint64_t generation_ops = 0;
        std::uint64_t dropped_packets = 0;
    };

    double percentile(std::vector<double> values, double p)
    {
        if (values.empty()) return 0;
        std::sort(values.begin(), values.end());
        const auto index = static_cast<std::size_t>(
            (values.size() - 1) * p
        );
        return values[index];
    }

    result measure(
        const char* name,
        double seconds,
        proxy_state& proxy,
        std::atomic<std::uint64_t>& generation_ops,
        int loss,
        int delay,
        bool render_stall
    )
    {
        proxy.loss.store(loss, std::memory_order_relaxed);
        proxy.delay_ms.store(delay, std::memory_order_relaxed);

        result output;
        output.name = name;
        output.frame_ms.reserve(10000);

        const auto initial_simulation = clt::prediction::ticks();
        const auto initial_snapshots =
            clt::connection::get()->player_snapshots.load(std::memory_order_relaxed);
        const auto initial_server =
            clt::connection::get()->server_tick.load(std::memory_order_relaxed);
        const auto initial_generation = generation_ops.load(std::memory_order_relaxed);
        const auto initial_dropped = proxy.dropped.load(std::memory_order_relaxed);

        const auto started = clock::now();
        std::size_t frame = 0;

        while (std::chrono::duration<double>(clock::now() - started).count() < seconds)
        {
            clt::window::poll_events();

            if (clt::window::should_close())
            {
                throw std::runtime_error("Workload window closed");
            }

            clt::prediction::controls(
                (frame / 120) % 2 ? ::player::left : ::player::forward, 0
            );

            const auto before = clock::now();

            if (!clt::window::minimized())
            {
                clt::renderer::draw_frame();
            }

            if (render_stall)
            {
                std::this_thread::sleep_for(80ms);
            }

            output.frame_ms.push_back(
                std::chrono::duration<double, std::milli>(
                    clock::now() - before
                ).count()
            );

            output.max_correction = std::max(
                output.max_correction, double(clt::prediction::correction())
            );

            ++frame;
        }

        output.seconds = std::chrono::duration<double>(
            clock::now() - started
        ).count();

        const auto final_server =
            clt::connection::get()->server_tick.load(std::memory_order_relaxed);

        output.simulation_hz = double(clt::prediction::ticks() - initial_simulation)
            / output.seconds;
        output.snapshots_hz =
            double(clt::connection::get()->player_snapshots.load(std::memory_order_relaxed)
                   - initial_snapshots) / output.seconds;
        output.server_hz = double(std::uint32_t(final_server - initial_server))
            / output.seconds;
        output.generation_ops = generation_ops.load(std::memory_order_relaxed)
            - initial_generation;
        output.dropped_packets = proxy.dropped.load(std::memory_order_relaxed)
            - initial_dropped;

        return output;
    }

    void report(const result& value)
    {
        const double fps = value.frame_ms.size() / value.seconds;
        const double average = value.frame_ms.empty() ? 0 :
            std::accumulate(value.frame_ms.begin(), value.frame_ms.end(), 0.0)
            / value.frame_ms.size();

        std::cout << std::fixed << std::setprecision(2)
                  << std::left << std::setw(18) << value.name
                  << " fps=" << std::setw(8) << fps
                  << " frame_avg=" << std::setw(9) << average
                  << " p95=" << std::setw(9) << percentile(value.frame_ms, 0.95)
                  << " p99=" << std::setw(9) << percentile(value.frame_ms, 0.99)
                  << " movement_hz=" << std::setw(7) << value.simulation_hz
                  << " server_hz=" << std::setw(7) << value.server_hz
                  << " snapshots_hz=" << std::setw(7) << value.snapshots_hz
                  << " correction_m=" << std::setw(7) << value.max_correction
                  << " gen_ops=" << value.generation_ops
                  << " lost=" << value.dropped_packets << '\n';
    }
} // namespace

int main()
{
    proxy_state proxy;
    std::atomic<bool> generate{false};
    std::atomic<std::uint64_t> generation_ops{0};
    std::thread proxy_thread;
    std::thread server_thread;
    std::mutex worker_error_mutex;
    std::string worker_error;
    int exit_code = 0;

    try
    {
        game::application::init();

        game::options::get()->port = server_port;
        game::options::get()->bind_host = "127.0.0.1";
        game::options::get()->duration = 0;

        srv::application::init();
        const auto count = instance_count();
        build_stress_scene(count);

        std::cout << "WORKLOAD actual Vulkan + UDP + movement + generation\n"
                  << "  instances=" << srv::application::get()->scene.instances.size()
                  << " scene_bytes=" << srv::application::get()->scene_bytes.size()
                  << " (GPU measurements are frame wall-time, not GPU timestamps)\n";

        server_thread = std::thread([&]
        {
            try
            {
                srv::application::run();
            }
            catch (const std::exception& e)
            {
                {
                    std::lock_guard<std::mutex> lock(worker_error_mutex);
                    worker_error = e.what();
                }
                game::application::get()->stop.store(true, std::memory_order_relaxed);
            }
        });

        proxy_thread = std::thread([&]
        {
            try
            {
                proxy_run(proxy);
            }
            catch (const std::exception& e)
            {
                {
                    std::lock_guard<std::mutex> lock(worker_error_mutex);
                    worker_error = e.what();
                }
                game::application::get()->stop.store(true, std::memory_order_relaxed);
            }
        });

        clt::connection::get()->host = "127.0.0.1";
        clt::connection::get()->port = proxy_port;
        clt::connection::init();

        clt::window::init();
        clt::input::init();
        clt::camera::init();
        clt::prediction::init();
        clt::renderer::init("shd/mesh.vert.spv", "shd/mesh.frag.spv");

        const auto deadline = clock::now() + 45s;

        while ((!clt::connection::snapshot() || clt::prediction::ticks() < 10)
               && clock::now() < deadline)
        {
            clt::window::poll_events();
            clt::renderer::draw_frame();
        }

        if (!clt::connection::snapshot() || clt::prediction::ticks() < 10)
        {
            throw std::runtime_error("Scene or player state did not arrive in 45 seconds");
        }

        const auto baseline = measure(
            "baseline", 4.0, proxy, generation_ops, 0, 0, false
        );
        report(baseline);

        const auto blocked_render = measure(
            "render_stall_80ms", 4.0, proxy, generation_ops, 0, 0, true
        );
        report(blocked_render);

        generate.store(true, std::memory_order_relaxed);

        if (!srv::generation::submit([&]
            {
                std::vector<std::uint32_t> data(1 << 20, 1u);
                std::uint32_t checksum = 12345;

                while (generate.load(std::memory_order_relaxed))
                {
                    for (auto& item : data)
                    {
                        checksum = (checksum ^ item) * 16777619u + 1u;
                        item = checksum;
                    }

                    generation_ops.fetch_add(data.size(), std::memory_order_relaxed);
                }

                // Ensure the work is observable and cannot simply disappear.
                generation_ops.fetch_add(checksum & 1u, std::memory_order_relaxed);
                return srv::generation::completion{};
            }))
        {
            throw std::runtime_error("Generation worker rejected stress job");
        }

        const auto generation = measure(
            "generation", 4.0, proxy, generation_ops, 0, 0, false
        );
        report(generation);

        const auto combined = measure(
            "generation+net", 6.0, proxy, generation_ops, 25, 50, false
        );
        report(combined);

        const auto recovery = measure(
            "recovery", 4.0, proxy, generation_ops, 0, 0, false
        );
        report(recovery);

        generate.store(false, std::memory_order_relaxed);

        if (blocked_render.simulation_hz < 45.0 ||
            combined.simulation_hz < 45.0 ||
            recovery.simulation_hz < 45.0)
        {
            std::cerr << "FAIL: Movement thread fell below 45 Hz during stress\n";
            exit_code = 1;
        }

        if (combined.snapshots_hz < 2.0 || recovery.snapshots_hz < 15.0)
        {
            std::cerr << "FAIL: Movement network snapshots stalled\n";
            exit_code = 1;
        }

        std::cout << "WORKLOAD " << (exit_code ? "FAIL" : "PASS") << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << "WORKLOAD FAIL: " << error.what() << '\n';
        exit_code = 1;
    }

    generate.store(false, std::memory_order_relaxed);
    proxy.loss.store(0, std::memory_order_relaxed);
    proxy.delay_ms.store(0, std::memory_order_relaxed);

    clt::prediction::shutdown();
    clt::renderer::shutdown();
    clt::input::shutdown();
    clt::window::shutdown();
    clt::connection::shutdown();

    proxy.stop.store(true, std::memory_order_relaxed);
    game::application::get()->stop.store(true, std::memory_order_relaxed);

    if (proxy_thread.joinable()) proxy_thread.join();
    if (server_thread.joinable()) server_thread.join();

    game::application::shutdown();

    if (!worker_error.empty())
    {
        std::cerr << "WORKLOAD worker: " << worker_error << '\n';
        exit_code = 1;
    }

    return exit_code;
}
