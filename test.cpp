#include "clt/input/input.hpp"
#include "clt/network/connection.hpp"
#include "clt/platform/window.hpp"
#include "clt/player/prediction.hpp"
#include "clt/render/renderer.hpp"
#include "clt/render/passes.hpp"
#include "clt/render/scene_renderer.hpp"
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
#include <unordered_map>
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

        if (parsed < 1 || parsed > net::scene_stream::max_scene_bytes / 48)
        {
            throw std::invalid_argument(
                "WORKLOAD_INSTANCES exceeds the scene protocol byte budget"
            );
        }

        return parsed;
    }

    void build_stress_scene(std::size_t count)
    {
        auto& server = *srv::application::get();

        // Only the workload scene gets an extended floor. The real test scene
        // remains small, while the player can walk through the entire benchmark.
        const float extent = std::max(
            500.0f, 100.0f + float((count + 199) / 200) * 1.6f
        );
        for (auto& mesh : server.scene.meshes)
        {
            if (mesh.id != 1)
            {
                continue;
            }

            for (auto& vertex : mesh.vertices)
            {
                vertex.position.x = std::copysign(extent, vertex.position.x);
                vertex.position.z = std::copysign(extent, vertex.position.z);
            }
        }

        // The server publishes a small spawn scene immediately. Background
        // generation appends independent instance batches as they finish.
        server.colliders = ::player::build_colliders(server.scene);
        server.collision_index = ::player::build_index(server.colliders);
        srv::application::begin_stream();
    }


    using vec3 = vk::util::math::vec3;
    using mat4 = vk::util::math::mat4;

    struct render_sample
    {
        double record_ms = 0;
        double fence_ms = 0;
        double upload_ms = 0;
        double gpu_ms = -1;
        clt::passes::timings stages{};
    };

    // Test-only version of the public renderer frame path. Query commands surround
    // the real scene_renderer::draw(), not an artificial GPU workload.
    struct gpu_profiler
    {
        vk::QueryPool queries{};
        std::uint32_t valid_bits = 0;
        float period = 0;
        bool pending = false;
        std::uint32_t last_image = 0;

        void init()
        {
            const auto families =
                vk::physical_device::get()->handle.getQueueFamilyProperties();
            const auto family = vk::queue::get()->graphics_family;
            if (family >= families.size() || !families[family].timestampValidBits)
            {
                std::cout << "GPU timestamp queries unsupported; CPU-only timings\n";
                return;
            }

            valid_bits = families[family].timestampValidBits;
            period = vk::physical_device::get()->handle.getProperties().limits.timestampPeriod;

            try
            {
                vk::QueryPoolCreateInfo info{};
                info.queryType = vk::QueryType::eTimestamp;
                info.queryCount = 2;
                queries = vk::query::create(info);
            }
            catch (const std::exception&)
            {
                std::cout << "GPU timestamp query creation failed; CPU-only timings\n";
                queries = nullptr;
            }
        }

        void shutdown()
        {
            if (queries)
            {
                vk::query::destroy(queries);
                queries = nullptr;
            }
        }

        render_sample frame()
        {
            render_sample timing;
            auto& render = *clt::renderer::get();
            auto& frame = render.frames[0];
            const auto device = vk::device::get()->handle;

            int width = 0, height = 0;
            clt::window::framebuffer_size(width, height);
            if (width <= 0 || height <= 0)
            {
                return timing;
            }

            const auto caps = vk::surface::capabilities();
            const auto extent = vk::swapchain::get()->extent;
            const auto target = caps.currentExtent.width == UINT32_MAX
                ? vk::Extent2D{
                    std::clamp(static_cast<std::uint32_t>(width),
                               caps.minImageExtent.width, caps.maxImageExtent.width),
                    std::clamp(static_cast<std::uint32_t>(height),
                               caps.minImageExtent.height, caps.maxImageExtent.height)}
                : caps.currentExtent;

            if (target != extent)
            {
                clt::renderer::resize();
                pending = false;
                return timing;
            }

            auto mark = clock::now();
            if (device.waitForFences(frame.fence, true, UINT64_MAX) !=
                vk::Result::eSuccess)
            {
                throw std::runtime_error("Workload frame fence failed");
            }
            timing.fence_ms = std::chrono::duration<double, std::milli>(
                clock::now() - mark).count();

            if (queries && pending)
            {
                std::array<std::uint64_t, 2> values{};
                const auto result = device.getQueryPoolResults(
                    queries, 0, 2, sizeof(values), values.data(),
                    sizeof(std::uint64_t), vk::QueryResultFlagBits::e64 |
                                           vk::QueryResultFlagBits::eWait
                );

                if (result == vk::Result::eSuccess)
                {
                    const std::uint64_t mask = valid_bits >= 64
                        ? ~std::uint64_t(0)
                        : (std::uint64_t(1) << valid_bits) - 1;
                    const auto elapsed_ticks = (values[1] - values[0]) & mask;
                    timing.gpu_ms = double(elapsed_ticks) * double(period) / 1000000.0;
                }
            }
            if (pending)
            {
                timing.stages = clt::passes::read(last_image);
            }
            pending = false;

            mark = clock::now();
            if (clt::scene_renderer::needs_sync())
            {
                for (const auto& context : render.frames)
                {
                    if (device.waitForFences(context.fence, true, UINT64_MAX) !=
                        vk::Result::eSuccess)
                        throw std::runtime_error("Workload scene synchronization failed");
                }
                clt::scene_renderer::synchronize();
            }
            timing.upload_ms = std::chrono::duration<double, std::milli>(
                clock::now() - mark).count();

            std::uint32_t image = 0;
            try
            {
                image = vk::swapchain::acquire(UINT64_MAX, frame.image_ready).value;
            }
            catch (const vk::OutOfDateKHRError&)
            {
                clt::renderer::resize();
                return timing;
            }

            if (render.image_in_flight[image] &&
                device.waitForFences(render.image_in_flight[image], true, UINT64_MAX) !=
                    vk::Result::eSuccess)
            {
                throw std::runtime_error("Workload swapchain fence failed");
            }
            for (auto& pending : render.image_in_flight)
                if (pending == frame.fence) pending = nullptr;
            render.image_in_flight[image] = frame.fence;
            device.resetFences(frame.fence);
            frame.command.reset();
            frame.command.begin(vk::CommandBufferBeginInfo{});

            if (queries)
            {
                frame.command.resetQueryPool(queries, 0, 2);
                frame.command.writeTimestamp(
                    vk::PipelineStageFlagBits::eTopOfPipe, queries, 0);
            }

            mark = clock::now();
            clt::passes::record(frame.command, image);
            last_image = image;
            timing.record_ms = std::chrono::duration<double, std::milli>(
                clock::now() - mark).count();

            if (queries)
            {
                frame.command.writeTimestamp(
                    vk::PipelineStageFlagBits::eBottomOfPipe, queries, 1);
                pending = true;
            }

            frame.command.end();

            vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
            vk::SubmitInfo submit{};
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &frame.image_ready;
            submit.pWaitDstStageMask = &stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &frame.command;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &render.render_ready[image];
            vk::submit::queue(vk::queue::get()->graphics, {submit}, frame.fence);

            vk::PresentInfoKHR present{};
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &render.render_ready[image];
            present.swapchainCount = 1;
            present.pSwapchains = &vk::swapchain::get()->handle;
            present.pImageIndices = &image;

            try
            {
                const auto result = vk::submit::present(
                    vk::queue::get()->present, present);
                if (result == vk::Result::eErrorOutOfDateKHR ||
                    result == vk::Result::eSuboptimalKHR)
                {
                    clt::renderer::resize();
                    pending = false;
                }
            }
            catch (const vk::OutOfDateKHRError&)
            {
                clt::renderer::resize();
                pending = false;
            }

            return timing;
        }
    };

    struct gpu_validation {
        bool available = false;
        std::size_t expected = 0;
        std::size_t actual = 0;
        std::size_t missing = 0;
        std::size_t unexpected = 0;
        std::size_t duplicates = 0;
        std::size_t out_of_range = 0;
        std::size_t bad_commands = 0;

        bool passed() const
        {
            return available && expected == actual && !missing &&
                !unexpected && !duplicates && !out_of_range && !bad_commands;
        }
    };

    gpu_validation validate_gpu_visibility()
    {
        const auto data = clt::scene_renderer::culling_data();
        gpu_validation result;
        if (!data.valid)
        {
            return result;
        }
        result.available = true;

        for (const auto& mesh : data.meshes)
        {

            // GPU outputs are device-local. Copy them into benchmark-only
            // staging buffers after the caller's GPU idle/fence synchronization.
            const auto device = vk::device::get()->handle;
            auto indirect = vk::buffer::create(
                sizeof(vk::DrawIndexedIndirectCommand),
                vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent
            );
            auto visible = vk::buffer::create(
                std::max<std::size_t>(
                    mesh.bounds->size() * sizeof(std::uint32_t), sizeof(std::uint32_t)
                ),
                vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent
            );
            const auto command = vk::command::begin_one_time();
            vk::MemoryBarrier ready{};
            ready.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
            ready.dstAccessMask = vk::AccessFlagBits::eTransferRead;
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eComputeShader,
                vk::PipelineStageFlagBits::eTransfer, {}, {ready}, {}, {}
            );
            vk::BufferCopy command_copy{};
            command_copy.size = sizeof(vk::DrawIndexedIndirectCommand);
            command.copyBuffer(mesh.indirect->handle, indirect.handle, {command_copy});
            vk::BufferCopy ids_copy{};
            ids_copy.size = mesh.bounds->size() * sizeof(std::uint32_t);
            if (ids_copy.size)
            {
                command.copyBuffer(mesh.visible->handle, visible.handle, {ids_copy});
            }
            command.end();
            const auto fence = device.createFence({});
            vk::SubmitInfo submit{};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            vk::submit::queue(vk::queue::get()->graphics, {submit}, fence);
            if (device.waitForFences(fence, true, UINT64_MAX) != vk::Result::eSuccess)
            {
                throw std::runtime_error("GPU visibility readback fence failed");
            }
            device.destroyFence(fence);
            vk::command::free({command});

            const auto* draw = static_cast<const vk::DrawIndexedIndirectCommand*>(
                vk::buffer::map(indirect)
            );
            const std::size_t actual = draw->instanceCount;
            result.actual += actual;
            if (draw->indexCount != mesh.index_count || draw->firstIndex ||
                draw->vertexOffset || draw->firstInstance)
            {
                ++result.bad_commands;
            }

            std::vector<std::uint8_t> expected(mesh.bounds->size(), 0);
            for (std::size_t i = 0; i < mesh.bounds->size(); ++i)
            {
                const auto& sphere = (*mesh.bounds)[i];
                bool visible = true;
                for (const auto& plane : data.planes)
                {
                    const float distance = plane.x * sphere[0] +
                        plane.y * sphere[1] + plane.z * sphere[2] + plane.w;
                    if (distance < -sphere[3])
                    {
                        visible = false;
                        break;
                    }
                }
                expected[i] = std::uint8_t(visible);
                result.expected += visible;
            }

            const auto* ids = static_cast<const std::uint32_t*>(
                vk::buffer::map(visible)
            );
            const std::size_t readable = std::min(actual, mesh.bounds->size());
            result.out_of_range += actual - readable;

            for (std::size_t i = 0; i < readable; ++i)
            {
                const auto id = ids[i];
                if (id >= expected.size())
                {
                    ++result.out_of_range;
                }
                else if (expected[id] == 0)
                {
                    ++result.unexpected;
                }
                else if (expected[id] == 2)
                {
                    ++result.duplicates;
                }
                else
                {
                    expected[id] = 2;
                }
            }

            for (auto value : expected)
            {
                result.missing += value == 1;
            }

            vk::buffer::destroy(visible);
            vk::buffer::destroy(indirect);
        }

        return result;
    }

    // Independent visibility audit: reconstruct the same six Vulkan frustum
    // planes and per-instance bounding spheres as the production renderer.
    struct visibility_probe
    {
        struct entry
        {
            std::uint32_t mesh = 0;
            vec3 center;
            float radius = 0;
        };

        struct summary
        {
            std::size_t visible = 0;
            std::size_t draw_calls = 0;
        };

        std::vector<entry> entries;

        explicit visibility_probe(const ::scene::state& scene)
        {
            struct bounds { vec3 center; float radius = 0; };
            std::unordered_map<std::uint32_t, bounds> meshes;

            for (const auto& mesh : scene.meshes)
            {
                if (mesh.vertices.empty() || mesh.indices.empty()) continue;
                vec3 low = mesh.vertices.front().position;
                vec3 high = low;
                for (const auto& v : mesh.vertices)
                {
                    const auto p = v.position;
                    low = {std::min(low.x,p.x),std::min(low.y,p.y),std::min(low.z,p.z)};
                    high = {std::max(high.x,p.x),std::max(high.y,p.y),std::max(high.z,p.z)};
                }
                meshes[mesh.id] = {(low + high) * 0.5f,
                                   vk::util::math::length((high - low) * 0.5f)};
            }

            entries.reserve(scene.instances.size());
            for (const auto& object : scene.instances)
            {
                const auto it = meshes.find(object.mesh_id);
                if (it == meshes.end()) continue;
                const auto& pose = object.pose;
                const vec3 center{
                    it->second.center.x * pose.scale.x,
                    it->second.center.y * pose.scale.y,
                    it->second.center.z * pose.scale.z
                };
                const float size = std::max({
                    std::abs(pose.scale.x), std::abs(pose.scale.y),
                    std::abs(pose.scale.z)
                });
                entries.push_back({
                    object.mesh_id,
                    pose.position + vk::util::math::rotate(pose.rotation, center),
                    it->second.radius * size
                });
            }
        }

        summary sample() const
        {
            const auto extent = vk::swapchain::get()->extent;
            const mat4 m = clt::camera::projection(
                float(extent.width) / float(extent.height)) * clt::camera::view();

            struct plane { vec3 n; float d; float len; };
            const auto make = [&](int row, float sign)
            {
                plane p{
                    {m(3,0)+sign*m(row,0), m(3,1)+sign*m(row,1),
                     m(3,2)+sign*m(row,2)},
                    m(3,3)+sign*m(row,3), 0
                };
                p.len = vk::util::math::length(p.n);
                return p;
            };
            plane near{{m(2,0),m(2,1),m(2,2)},m(2,3),0};
            near.len = vk::util::math::length(near.n);
            const std::array<plane, 6> planes{
                make(0,1),make(0,-1),make(1,1),make(1,-1),near,make(2,-1)
            };

            summary result;
            std::vector<std::uint32_t> drawn;
            for (const auto& instance : entries)
            {
                bool inside = true;
                for (const auto& p : planes)
                {
                    if (vk::util::math::dot(p.n, instance.center) + p.d <
                        -instance.radius * p.len)
                    {
                        inside = false;
                        break;
                    }
                }
                if (!inside) continue;
                ++result.visible;
                if (std::find(drawn.begin(), drawn.end(), instance.mesh) ==
                    drawn.end())
                {
                    drawn.push_back(instance.mesh);
                }
            }
            result.draw_calls = drawn.size();
            return result;
        }
    };

    // Timing probes use copies of player state and the same collision bounds.
    // They never mutate the live server or client simulation.
    struct collision_probe
    {
        double query_ms = 0;
        double step_ms = 0;

        collision_probe(
            const ::player::collision_world& bounds,
            const spatial::index& index
        )
        {
            constexpr int repeats = 32;
            std::uint64_t hits = 0;
            const ::player::state player;
            auto start = clock::now();

            const spatial::bounds region{
                {player.position.x - 0.3f, player.position.y,
                 player.position.z - 0.3f},
                {player.position.x + 0.3f, player.position.y + 1.8f,
                 player.position.z + 0.3f}
            };
            for (int run = 0; run < repeats; ++run)
            {
                index.query(region, [&](std::uint32_t id)
                {
                    if (id < bounds.size())
                    {
                        ++hits;
                    }
                });
            }
            query_ms = std::chrono::duration<double, std::milli>(
                clock::now() - start).count() / repeats;

            const ::player::input movement{1, ::player::forward, 0};
            start = clock::now();
            for (int run = 0; run < repeats; ++run)
            {
                auto state = player;
                ::player::step(state, movement, bounds, &index);
                hits += std::uint64_t(state.grounded);
            }
            step_ms = std::chrono::duration<double, std::milli>(
                clock::now() - start).count() / repeats;

            if (hits == UINT64_MAX)
                std::cerr << "Unexpected collision accumulator\n";
        }
    };

    struct result
    {
        std::string name;
        std::vector<double> frame_ms;
        std::vector<double> record_ms;
        std::vector<double> fence_ms;
        std::vector<double> upload_ms;
        std::vector<double> gpu_ms;
        std::array<std::vector<double>, 4> pass_ms;
        std::size_t gpu_candidates = 0;
        std::size_t visible_instances = 0;
        std::size_t draw_calls = 0;
        double collision_query_ms = 0;
        double collision_step_ms = 0;
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
        bool render_stall,
        gpu_profiler& gpu,
        const visibility_probe& visibility,
        const collision_probe& collision,
        bool stationary = false
    )
    {
        proxy.loss.store(loss, std::memory_order_relaxed);
        proxy.delay_ms.store(delay, std::memory_order_relaxed);

        result output;
        output.name = name;
        output.frame_ms.reserve(10000);
        output.record_ms.reserve(10000);
        output.fence_ms.reserve(10000);
        output.upload_ms.reserve(10000);
        output.gpu_ms.reserve(10000);
        for (auto& pass : output.pass_ms) pass.reserve(10000);

        // Visibility counts are sampled once per phase, not every frame; the
        // audit must not dominate the renderer's actual frame-time measurement.
        const auto counts = visibility.sample();
        const auto snapshot = clt::connection::snapshot();
        output.gpu_candidates = snapshot ? snapshot->instances.size() : 0;
        output.visible_instances = counts.visible;
        output.draw_calls = counts.draw_calls;
        output.collision_query_ms = collision.query_ms;
        output.collision_step_ms = collision.step_ms;

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
            clt::connection::check_error();
            clt::window::poll_events();

            if (clt::window::should_close())
            {
                throw std::runtime_error("Workload window closed");
            }

            clt::prediction::controls(
                stationary ? 0 :
                ((frame / 120) % 2 ? ::player::left : ::player::forward), 0
            );

            const auto before = clock::now();

            if (!clt::window::minimized())
            {
                const auto measured = gpu.frame();
                output.record_ms.push_back(measured.record_ms);
                output.fence_ms.push_back(measured.fence_ms);
                output.upload_ms.push_back(measured.upload_ms);
                if (measured.gpu_ms >= 0)
                {
                    output.gpu_ms.push_back(measured.gpu_ms);
                }
                if (measured.stages.valid)
                {
                    for (std::size_t i = 0; i < output.pass_ms.size(); ++i)
                    {
                        output.pass_ms[i].push_back(measured.stages.milliseconds[i]);
                    }
                }
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

    double average(const std::vector<double>& samples)
    {
        return samples.empty() ? 0 :
            std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
    }

    void report(const result& value)
    {
        const double fps = value.frame_ms.size() / value.seconds;
        const double frame_avg = average(value.frame_ms);

        std::cout << std::fixed << std::setprecision(2)
                  << std::left << std::setw(18) << value.name
                  << " fps=" << std::setw(8) << fps
                  << " frame_avg=" << std::setw(9) << frame_avg
                  << " p95=" << std::setw(9) << percentile(value.frame_ms, 0.95)
                  << " p99=" << std::setw(9) << percentile(value.frame_ms, 0.99)
                  << " movement_hz=" << std::setw(7) << value.simulation_hz
                  << " server_hz=" << std::setw(7) << value.server_hz
                  << " snapshots_hz=" << std::setw(7) << value.snapshots_hz
                  << " correction_m=" << std::setw(7) << value.max_correction
                  << " gen_ops=" << value.generation_ops
                  << " lost=" << value.dropped_packets << '\n';

        std::cout << "  cpu_record_ms=" << average(value.record_ms)
                  << " p95=" << percentile(value.record_ms, 0.95)
                  << " fence_ms=" << average(value.fence_ms)
                  << " upload_ms=" << average(value.upload_ms)
                  << " gpu_draw_ms=";
        if (value.gpu_ms.empty())
        {
            std::cout << "n/a";
        }
        else
        {
            std::cout << average(value.gpu_ms)
                      << " gpu_p95=" << percentile(value.gpu_ms, 0.95);
        }
        std::cout << " visible=" << value.visible_instances
                  << " draws=" << value.draw_calls
                  << " aabb_query_ms=" << value.collision_query_ms
                  << " physics_step_ms=" << value.collision_step_ms << '\n';
        std::cout << "  gpu_candidates=" << value.gpu_candidates
                  << " visibility=gpu_indirect";
        const auto& passes = clt::passes::ordered();
        for (std::size_t i = 0; i < value.pass_ms.size(); ++i)
        {
            std::cout << " " << passes[i].name << "_gpu_ms=";
            if (value.pass_ms[i].empty()) std::cout << "n/a";
            else std::cout << average(value.pass_ms[i]);
        }
        std::cout << '\n';
    }
} // namespace

int main()
{
    // Keep the last completed workload phase visible even after a driver fault.
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    proxy_state proxy;
    std::atomic<bool> generate{false};
    std::atomic<std::uint64_t> generation_ops{0};
    std::thread proxy_thread;
    std::thread server_thread;
    std::mutex worker_error_mutex;
    std::string worker_error;
    int exit_code = 0;
    gpu_profiler gpu;
    auto startup_mark = clock::now();
    const auto startup_begin = startup_mark;
    const auto milestone = [&](const char* label)
    {
        const auto now = clock::now();
        std::cout << "STARTUP " << label << " "
                  << std::chrono::duration<double, std::milli>(
                         now - startup_mark).count() << " ms\n" << std::flush;
        startup_mark = now;
    };

    try
    {
        game::application::init();

        game::options::get()->port = server_port;
        game::options::get()->bind_host = "127.0.0.1";
        game::options::get()->duration = 0;

        srv::application::init();
        milestone("server_init");

        const auto count = instance_count();
        build_stress_scene(count);
        milestone("initial_scene");

        // The existing generation worker builds generic scene::state patches.
        // Only completed patches are committed by the server's 60 Hz loop.
        constexpr std::size_t batch_size = 4096;
        for (std::size_t start = 0; start < count; start += batch_size)
        {
            const auto end = std::min(start + batch_size, count);
            if (!srv::generation::submit([start, end, count]
            {
                ::scene::state patch;
                patch.instances.reserve(end - start);
                for (std::size_t i = start; i < end; ++i)
                {
                    ::scene::instance cube;
                    cube.id = static_cast<std::uint32_t>(100 + i);
                    cube.mesh_id = 2;
                    const float x = float(int(i % 200) - 100) * 1.6f;
                    const float z = -float(i / 200) * 1.6f - 8.0f;
                    cube.pose.position = {x, 0.5f, z};
                    patch.instances.push_back(cube);
                }

                return [patch = std::move(patch), last = end == count]() mutable
                {
                    srv::application::append_scene(std::move(patch), last);
                };
            }))
            {
                throw std::runtime_error("Generation worker queue full");
            }
        }

        milestone("generation_jobs_queued");
        std::cout << "WORKLOAD actual Vulkan + UDP + movement + generation\n"
                  << "  target_instances=" << (count + 4)
                  << " initial_scene_bytes=" << srv::application::get()->scene_bytes.size()
                  << " (progressive scene batches, GPU timestamps where supported)\n";

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

        milestone("server_proxy_threads_started");
        clt::connection::get()->host = "127.0.0.1";
        clt::connection::get()->port = proxy_port;
        clt::connection::init();
        milestone("network_threads");

        // Measure uncapped presentation in the workload. The normal game
        // stays VSynced unless launched with VK_UNCAPPED=1.
        clt::renderer::get()->uncapped = true;
        clt::window::init();
        clt::input::init();
        clt::camera::init();
        clt::prediction::init();
        clt::renderer::init("shd/mesh.vert.spv", "shd/mesh.frag.spv");
        const auto mode = clt::renderer::get()->present_mode;
        std::cout << "PRESENT mode="
                  << (mode == vk::PresentModeKHR::eImmediate ? "immediate" :
                      mode == vk::PresentModeKHR::eMailbox ? "mailbox" : "fifo")
                  << " uncapped_requested=" << clt::renderer::get()->uncapped << '\n';
        milestone("window+vulkan_init");
        gpu.init();

        const auto deadline = clock::now() + std::max(
            60'000ms, std::chrono::milliseconds(30'000 + count * 2)
        );
        auto next_progress = clock::now();

        while ((!clt::connection::get()->scene_complete.load(std::memory_order_relaxed) ||
                clt::prediction::ticks() < 10) && clock::now() < deadline)
        {
            clt::connection::check_error();
            clt::window::poll_events();

            if (clock::now() >= next_progress)
            {
                const auto partial = clt::connection::snapshot();
                visibility_probe::summary counts{};
                if (partial)
                {
                    counts = visibility_probe(*partial).sample();
                }

                std::cout << "STREAM received="
                          << (partial ? partial->instances.size() : 0)
                          << "/" << (count + 4)
                          << " visible=" << counts.visible
                          << " draw_calls=" << counts.draw_calls
                          << " complete=" << clt::connection::get()->scene_complete.load(
                              std::memory_order_relaxed) << '\n' << std::flush;
                next_progress = clock::now() + 1s;
            }

            clt::renderer::draw_frame();
        }

        if (!clt::connection::get()->scene_complete.load(std::memory_order_relaxed) ||
            clt::prediction::ticks() < 10)
        {
            throw std::runtime_error("Scene stream did not finish before startup deadline");
        }
        const auto complete_scene = clt::connection::snapshot();
        if (!complete_scene || complete_scene->instances.size() != count + 4)
        {
            throw std::runtime_error("Scene transfer completed without the expected instances");
        }
        std::cout << "Scene received: " << complete_scene->meshes.size()
                  << " meshes, " << complete_scene->instances.size() << " instances\n";

        const auto audit_gpu = [&](const char* label)
        {
            const auto device = vk::device::get()->handle;
            vk::device::idle();

            const auto check = validate_gpu_visibility();
            std::cout << "GPU_VISIBILITY " << label
                      << " expected=" << check.expected
                      << " gpu_visible=" << check.actual
                      << " missing=" << check.missing
                      << " unexpected=" << check.unexpected
                      << " duplicates=" << check.duplicates
                      << " out_of_range=" << check.out_of_range
                      << " invalid_commands=" << check.bad_commands
                      << " result=" << (check.passed() ? "PASS" : "FAIL") << '\n';
            if (!check.passed())
            {
                throw std::runtime_error("GPU visible IDs differ from CPU sphere reference");
            }
        };

        // Render once after the full snapshot is published so the readback
        // verifies all streamed instances rather than the previous batch.
        clt::renderer::draw_frame();
        audit_gpu("full_scene");
        milestone("progressive_scene_stream+gpu_upload+first_player_ticks");
        std::cout << "STARTUP total "
                  << std::chrono::duration<double>(clock::now() - startup_begin).count()
                  << " s\n";

        const visibility_probe visibility(*clt::connection::snapshot());
        const collision_probe collision(
            srv::application::get()->colliders,
            srv::application::get()->collision_index
        );
        std::cout << "COLLISION query_ms=" << collision.query_ms
                  << " step_ms=" << collision.step_ms << '\n';

        const auto stationary = measure(
            "stationary", 2.0, proxy, generation_ops, 0, 0, false,
            gpu, visibility, collision, true
        );
        report(stationary);

        const auto baseline = measure(
            "baseline", 4.0, proxy, generation_ops, 0, 0, false,
            gpu, visibility, collision
        );
        report(baseline);

        const auto blocked_render = measure(
            "render_stall_80ms", 4.0, proxy, generation_ops, 0, 0, true,
            gpu, visibility, collision
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
            "generation", 4.0, proxy, generation_ops, 0, 0, false,
            gpu, visibility, collision
        );
        report(generation);

        const auto combined = measure(
            "generation+net", 6.0, proxy, generation_ops, 25, 50, false,
            gpu, visibility, collision
        );
        report(combined);

        const auto recovery = measure(
            "recovery", 4.0, proxy, generation_ops, 0, 0, false,
            gpu, visibility, collision
        );
        report(recovery);
        audit_gpu("after_movement");

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

    std::cout << "SHUTDOWN prediction\n";
    clt::prediction::shutdown();

    // The final command buffer can still reference the timestamp query pool.
    // Wait for Vulkan execution to finish before freeing that pool.
    std::cout << "SHUTDOWN gpu_idle\n";
    if (vk::device::get()->handle)
    {
        try
        {
            vk::device::idle();
        }
        catch (const std::exception& error)
        {
            std::cerr << "WORKLOAD GPU idle: " << error.what() << '\n';
            exit_code = 1;
        }
    }

    std::cout << "SHUTDOWN gpu_queries\n";
    gpu.shutdown();
    std::cout << "SHUTDOWN renderer\n";
    clt::renderer::shutdown();
    clt::input::shutdown();
    clt::window::shutdown();
    std::cout << "SHUTDOWN connection\n";
    clt::connection::shutdown();

    proxy.stop.store(true, std::memory_order_relaxed);
    game::application::get()->stop.store(true, std::memory_order_relaxed);

    std::cout << "SHUTDOWN workers\n";
    if (proxy_thread.joinable()) proxy_thread.join();
    if (server_thread.joinable()) server_thread.join();

    std::cout << "SHUTDOWN game\n";
    game::application::shutdown();

    if (!worker_error.empty())
    {
        std::cerr << "WORKLOAD worker: " << worker_error << '\n';
        exit_code = 1;
    }

    std::cout << "WORKLOAD EXIT " << exit_code << '\n';
    return exit_code;
}
