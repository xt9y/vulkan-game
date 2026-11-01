#include "srv/game/application.hpp"
#include "srv/game/options.hpp"
#include "clt/network/connection.hpp"
#include "clt/player/prediction.hpp"

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

    // Diagnostics remain in main.cpp and sample existing client counters.
    // Server ticks are echoed by network packets, not directly measured here.
    class diagnostics
    {
        using clock = std::chrono::steady_clock;
        std::atomic<bool> stopping{false};
        std::thread worker;

    public:
        diagnostics()
        {
            if (game::options::get()->mode == game::options::mode::server_only)
                return;

            worker = std::thread([this]
            {
                auto& network = *clt::connection::get();
                auto previous_time = clock::now();
                auto previous_server = network.server_tick.load(std::memory_order_relaxed);
                auto previous_snapshots = network.player_snapshots.load(std::memory_order_relaxed);
                auto previous_movement = clt::prediction::ticks();
                float maximum_correction = 0;

                while (!stopping.load(std::memory_order_relaxed))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    maximum_correction = std::max(
                        maximum_correction, clt::prediction::correction()
                    );
                    const auto now = clock::now();
                    const double seconds = std::chrono::duration<double>(
                        now - previous_time).count();
                    if (seconds < 1.0) continue;

                    const auto server = network.server_tick.load(std::memory_order_relaxed);
                    const auto snapshots = network.player_snapshots.load(std::memory_order_relaxed);
                    const auto movement = clt::prediction::ticks();

                    std::cout << std::fixed << std::setprecision(1)
                              << "[diag] server_echo="
                              << double(std::uint32_t(server - previous_server)) / seconds
                              << "Hz snapshots="
                              << double(snapshots - previous_snapshots) / seconds
                              << "Hz movement="
                              << double(movement - previous_movement) / seconds
                              << "Hz correction_max=" << maximum_correction << "m"
                              << " connected=" << network.connected.load(std::memory_order_relaxed)
                              << " scene_complete=" << network.scene_complete.load(
                                  std::memory_order_relaxed)
                              << '\n' << std::flush;

                    previous_server = server;
                    previous_snapshots = snapshots;
                    previous_movement = movement;
                    previous_time = now;
                    maximum_correction = 0;
                }
            });
        }

        void shutdown()
        {
            stopping.store(true, std::memory_order_relaxed);
            if (worker.joinable()) worker.join();
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
