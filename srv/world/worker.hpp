#pragma once

#include <functional>

namespace srv::generation
{
    // Compute off-thread; apply the returned callback on the server thread.
    using completion = std::function<void()>;
    using job = std::function<completion()>;

    void init();
    bool submit(job work);
    void commit();
    void shutdown();
} // namespace srv::generation
