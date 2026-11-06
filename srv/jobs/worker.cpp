#include "worker.hpp"
#include <jobs/pool.hpp>
#include <utility>
namespace srv::generation {
    namespace { jobs::pool& queue() { static jobs::pool instance; return instance; } }
    void init() { queue().start(1,64); }
    bool submit(job work) { return queue().submit(std::move(work)); }
    void commit() { queue().commit(2); }
    void shutdown() { queue().stop(); }
}
