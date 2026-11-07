#pragma once

#include "clt/input/input.hpp"
#include "srv/game/options.hpp"
#include <editor/editor.hpp>

namespace clt::application
{
    struct callbacks
    {
        // Main-thread game controls, after mouse look and movement prediction.
        void (*on_input)(const input::frame&) = nullptr;
        // Game owns spawning/property writes; editor only emits requests.
        void (*on_editor_action)(const ::editor::action&) = nullptr;
    };

    struct state
    {
        game::options::state options;
        callbacks hooks;
        bool initialized = false;
    };

    state* get();
    void set_callbacks(callbacks value);
    void init();
    int run();
    void shutdown() noexcept;
} // namespace clt::application
