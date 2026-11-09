#pragma once


namespace clt::application {
    struct state {
        bool initialized = false;
    };
    state* get();
    void init();
    int run();
    void shutdown() noexcept;

} // namespace clt::application
