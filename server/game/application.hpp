#pragma once
#include <atomic>

namespace game::application
{
struct state
{
    std::atomic<bool> stop{false};
};


state *get()
{
    static state data;

    return &data;
}

} // namespace game::application
