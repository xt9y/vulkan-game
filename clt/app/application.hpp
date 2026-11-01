#pragma once

#include "srv/game/options.hpp"

namespace clt::application
{
    struct state
    {
        game::options::state options;
        bool initialized = false;
    };

    state* get();
    void init();
    int run();
    void shutdown() noexcept;
} // namespace clt::application
