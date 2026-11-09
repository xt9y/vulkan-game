#pragma once
#include "../../server/player.hpp"
#include "connection.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vk/vk.hpp>

namespace clt::prediction
{

namespace
{
using clock = std::chrono::steady_clock;
using vec3 = vk::util::math::vec3;

std::atomic<bool> stop{false};
std::atomic<std::uint32_t> buttons{0};
std::atomic<float> yaw{0};
std::atomic<std::uint64_t> tick_count{0};
std::mutex pose_mutex;
vec3 pose{};
std::thread worker;
void run()
{
    std::shared_ptr<const ::scene::state> previous_scene;
    ::player::collision_world colliders;
    spatial::index index;
    ::player::state avatar{};
    std::deque<::player::input> pending;
    vec3 correction{};
    std::uint32_t sequence = 0, server_tick = 0;

    bool ready = false, configured = false;
    auto next = clock::now();

    constexpr auto dt = std::chrono::nanoseconds(1'000'000'000 / net::limits::tick_hz);
    while (!stop.load(std::memory_order_relaxed))
    {
        const auto scene = connection::snapshot();
        const auto connected = connection::get()->connected.load(std::memory_order_relaxed);
        const auto params = connection::movement_parameters();
        if (!connected || !scene || !params) {
            previous_scene.reset();
            colliders.clear();
            index = {};
            avatar = {};
            pending.clear();
            correction = {};
            sequence = server_tick = 0;
            ready = configured = false;
        }
        else {
            if (!configured) {
                ::player::configure(*params);
                avatar.position = params->spawn;
                configured = true;
            }
            if (scene != previous_scene) {
                // Edited instances can move or disappear without changing vector size.
                bool append = previous_scene && scene->meshes.size() == previous_scene->meshes.size() &&
                              scene->instances.size() > previous_scene->instances.size();
                if(append)
                    for(std::size_t i=0;i<previous_scene->instances.size();++i) {
                        const auto& a=previous_scene->instances[i];
                        const auto& b=scene->instances[i];
                        const auto& p=a.pose.position,&q=b.pose.position;
                        if(a.id!=b.id||a.enabled!=b.enabled||
                           p.x!=q.x||p.y!=q.y||p.z!=q.z||
                           a.pose.scale.x!=b.pose.scale.x||
                           a.pose.scale.y!=b.pose.scale.y||
                           a.pose.scale.z!=b.pose.scale.z||
                           a.pose.rotation.x!=b.pose.rotation.x||
                           a.pose.rotation.y!=b.pose.rotation.y||
                           a.pose.rotation.z!=b.pose.rotation.z||
                           a.pose.rotation.w!=b.pose.rotation.w) {
                            append=false;
                            break;
                        }
                    }
                if (append) {
                    const auto added = std::span<const ::scene::instance>(scene->instances).subspan(previous_scene->instances.size());
                    auto new_boxes = ::player::build_colliders(scene->meshes, added);
                    index.append(::player::collision_bounds(new_boxes));
                    colliders.insert(colliders.end(), new_boxes.begin(), new_boxes.end());
                }
                else {
                    colliders = ::player::build_colliders(*scene);
                    index = ::player::build_index(colliders);
                }
                previous_scene = scene;
            }
            if (const auto update = connection::take_authority();
                update && (!ready || net::message::newer(update->tick, server_tick)))
            {
                const auto before = avatar.position;
                const bool was_ready = ready;
                avatar = update->player;
                server_tick = update->tick;
                ready = true;
                while (!pending.empty() && !net::message::newer(pending.front().sequence, update->sequence))
                    pending.pop_front();
                for (const auto &cmd : pending)
                    ::player::step(avatar, cmd, colliders, &index);
                if (was_ready) {
                    const auto delta = before - avatar.position;
                    const auto distance = vk::util::math::length(delta);
                    correction = distance < 2.f ? correction + delta : vec3{};
                }
            }
            if (ready) {
                ::player::input cmd{};
                if (!(cmd.sequence = ++sequence)) cmd.sequence = ++sequence;
                cmd.buttons = buttons.load(std::memory_order_relaxed);
                cmd.yaw = yaw.load(std::memory_order_relaxed);
                ::player::step(avatar, cmd, colliders, &index);
                tick_count.fetch_add(1, std::memory_order_relaxed);
                pending.push_back(cmd);
                if (pending.size() > 128) pending.pop_front();
                connection::send_input(cmd);
            }
        }
        correction = correction * .8f;
        {
            std::lock_guard<std::mutex> lock(pose_mutex);
            pose = avatar.position + correction;
        }
        next += dt;
        if (next < clock::now()) next = clock::now();
        std::this_thread::sleep_until(next);
    }
}
} // namespace
void init()
{
    stop.store(false, std::memory_order_relaxed);
    buttons.store(0, std::memory_order_relaxed);
    yaw.store(0, std::memory_order_relaxed);
    tick_count.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(pose_mutex);
        pose = {};
    }
    worker = std::thread(run);
}
void controls(std::uint32_t b, float y)
{
    buttons.store(b, std::memory_order_relaxed);
    yaw.store(y, std::memory_order_relaxed);
}
vk::util::math::vec3 position()
{
    std::lock_guard<std::mutex> lock(pose_mutex);
    return pose;
}
std::uint64_t ticks() { return tick_count.load(std::memory_order_relaxed); }
void shutdown()
{
    stop.store(true, std::memory_order_relaxed);
    if (worker.joinable()) worker.join();
}

} // namespace clt::prediction
