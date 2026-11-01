#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "srv/app/application.hpp"
#include "clt/network/connection.hpp"
#include "clt/player/prediction.hpp"
#include "clt/render/renderer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <thread>

namespace
{
    static_assert(
        std::atomic<bool>::is_always_lock_free, "Signal shutdown requires lock-free atomics"
    );

    void stop(int)
    {
        game::application::get()->stop.store(true, std::memory_order_relaxed);
    }

    // All diagnostic output stays here. Worker threads only update atomics,
    // so logging cannot delay the renderer, network receiver or server tick.
    class diagnostics
    {
        using clock = std::chrono::steady_clock;
        std::atomic<bool> stopping{false};
        std::thread worker;

        void run(bool server, bool client)
        {
            auto* server_data = srv::application::get();
            auto* network = clt::connection::get();
            auto& render = clt::renderer::stats();

            auto previous_time = clock::now();
            auto previous_server = server_data->diagnostic_ticks.load(std::memory_order_relaxed);
            auto previous_frames = render.frames.load(std::memory_order_relaxed);
            auto previous_movement = clt::prediction::ticks();
            auto previous_snapshots = network->player_snapshots.load(std::memory_order_relaxed);
            float correction_peak = 0;

            while (!stopping.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (client)
                {
                    correction_peak = std::max(correction_peak, clt::prediction::correction());
                }

                const auto now = clock::now();
                const double seconds = std::chrono::duration<double>(
                    now - previous_time).count();
                if (seconds < 1.0)
                {
                    continue;
                }

                std::cout << std::fixed << std::setprecision(1) << "[diag]";

                if (server)
                {
                    const auto ticks = server_data->diagnostic_ticks.load(
                        std::memory_order_relaxed);
                    const auto delta = ticks - previous_server;
                    const auto work = server_data->diagnostic_work_us.exchange(
                        0, std::memory_order_relaxed);
                    const auto maximum = server_data->diagnostic_max_work_us.exchange(
                        0, std::memory_order_relaxed);
                    const auto overruns = server_data->diagnostic_overruns.exchange(
                        0, std::memory_order_relaxed);

                    std::cout << " srv=" << double(delta) / seconds << "Hz"
                              << " srv_work=" << (delta ? double(work) / delta / 1000.0 : 0.0)
                              << "ms"
                              << " srv_max=" << double(maximum) / 1000.0 << "ms"
                              << " srv_late=" << overruns;
                    previous_server = ticks;
                }

                if (client)
                {
                    const auto frames = render.frames.load(std::memory_order_relaxed);
                    const auto movement = clt::prediction::ticks();
                    const auto snapshots = network->player_snapshots.load(
                        std::memory_order_relaxed);
                    const auto fence_max = render.max_fence_wait_us.exchange(
                        0, std::memory_order_relaxed);
                    const auto frame_max = render.max_frame_us.exchange(
                        0, std::memory_order_relaxed);
                    const auto deferrals = render.deferred.exchange(
                        0, std::memory_order_relaxed);
                    const auto scene_updates = render.scene_updates.exchange(
                        0, std::memory_order_relaxed);
                    const auto snapshot_gap = network->diagnostic_max_snapshot_gap_us.exchange(
                        0, std::memory_order_relaxed);

                    std::cout << " fps=" << double(frames - previous_frames) / seconds
                              << " move=" << double(movement - previous_movement) / seconds
                              << "Hz"
                              << " snapshots=" << double(snapshots - previous_snapshots) / seconds
                              << "Hz"
                              << " net_gap_max=" << double(snapshot_gap) / 1000.0 << "ms"
                              << " correction_max=" << correction_peak << "m"
                              << " frame_max=" << double(frame_max) / 1000.0 << "ms"
                              << " fence_max=" << double(fence_max) / 1000.0 << "ms"
                              << " deferred=" << deferrals
                              << " scene_updates=" << scene_updates
                              << " connected=" << network->connected.load(std::memory_order_relaxed)
                              << " scene_complete=" << network->scene_complete.load(
                                  std::memory_order_relaxed);

                    previous_frames = frames;
                    previous_movement = movement;
                    previous_snapshots = snapshots;
                    correction_peak = 0;
                }

                std::cout << '\n' << std::flush;
                previous_time = now;
            }
        }

    public:
        diagnostics()
        {
            const auto mode = game::options::get()->mode;
            const bool server = mode != game::options::mode::client_only;
            const bool client = mode != game::options::mode::server_only;
            worker = std::thread([this, server, client] { run(server, client); });
        }

        void shutdown()
        {
            stopping.store(true, std::memory_order_relaxed);
            if (worker.joinable())
            {
                worker.join();
            }
        }

        ~diagnostics()
        {
            shutdown();
        }
    };
} // namespace

int main(int argc, char** argv)
{
    try
    {
        game::options::init(argc, argv);

        if (game::options::get()->help)
        {
            game::options::print_help();
            game::options::shutdown();
            return 0;
        }

        game::application::init();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop); 

        diagnostics diagnostic;
        const int result = game::application::run();
        diagnostic.shutdown();

        game::application::shutdown();
        game::options::shutdown();

        return result;
    }
    catch (const std::exception& error)
    {
        game::application::shutdown();
        game::options::shutdown();
        std::cerr << "vulkan-game: " << error.what() << '\n';

        return 1;
    }
}
