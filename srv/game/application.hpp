#pragma once

#include <atomic>
#include <future>

namespace game::application
{
    struct state
    {
        std::atomic<bool> stop{false};
        std::future<void> server_worker;
        bool initialized = false;
    };

    state* get();
    void init();
    int run();
    void shutdown() noexcept;
} // namespace game::application
