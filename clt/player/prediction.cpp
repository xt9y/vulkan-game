#include "prediction.hpp"

#include "clt/network/connection.hpp"
#include "srv/shared/net/message.hpp"
#include "srv/shared/player/movement.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include <cmath>

namespace clt::prediction
{
    namespace
    {
        using clock = std::chrono::steady_clock;

        struct state {
            std::atomic<bool> stop{false};

            std::atomic<std::uint32_t> buttons{0};
            std::atomic<float> yaw{0};
            std::atomic<std::uint64_t> ticks{0};
            std::atomic<float> error{0};

            std::mutex pose_mutex;
            vk::util::math::vec3 pose{0, 0, 4};

            std::thread worker;
        };

        state& get()
        {
            static state data;
            return data;
        }

        void run()
        {
            auto& data = get();

            std::shared_ptr<const ::scene::state> cached_scene;
            ::player::collision_world colliders;
            spatial::index collision_index;

            ::player::state avatar;
            vk::util::math::vec3 visual_offset{};
            std::deque<::player::input> pending;

            std::uint32_t sequence = 0;
            std::uint32_t last_server_tick = 0;

            bool ready = false;

            auto next = clock::now();
            constexpr auto tick = std::chrono::nanoseconds(1'000'000'000 / 60);

            while (!data.stop.load(std::memory_order_relaxed))
            {
                const auto source = connection::snapshot();
                const bool connected = connection::get()->connected.load(std::memory_order_relaxed);

                if (!connected || !source || source != cached_scene)
                {
                    cached_scene = source;

                    colliders = source
                        ? ::player::build_colliders(*source)
                        : ::player::collision_world{};
                    collision_index = ::player::build_index(colliders);        

                    avatar = {};
                    visual_offset = {};
                    pending.clear();

                    sequence = 0;
                    last_server_tick = 0;
                    ready = false;
                }

                if (connected && source)
                {
                    if (const auto update = connection::take_authority())
                    {
                        if (!ready || net::message::newer(update->tick, last_server_tick))
                        {
                            const auto previous = avatar.position;
                            const bool was_ready = ready;
                            avatar = update->player;
                            last_server_tick = update->tick;
                            ready = true;

                            while (!pending.empty() && !net::message::newer(pending.front().sequence, update->sequence))
                            {
                                pending.pop_front();
                            }

                            for (const auto& command : pending)
                            {
                                ::player::step(avatar, command, colliders, &collision_index);
                            }

                            if (was_ready)
                            {
                                const auto delta = previous - avatar.position;
                                const auto distance = vk::util::math::length(delta);
                                data.error.store(distance, std::memory_order_relaxed);
                                if (distance < 2.0f)
                                {
                                    visual_offset = visual_offset + delta;
                                }
                                else
                                {
                                    visual_offset = {};
                                }
                            }
                        }
                    }

                    if (ready)
                    {
                        ::player::input input;

                        input.sequence = ++sequence;

                        if (input.sequence == 0)
                        {
                            input.sequence = ++sequence;
                        }

                        input.buttons =
                            data.buttons.load(std::memory_order_relaxed);

                        input.yaw =
                            data.yaw.load(std::memory_order_relaxed);

                        ::player::step(avatar, input, colliders, &collision_index);
                        data.ticks.fetch_add(1, std::memory_order_relaxed);

                        pending.push_back(input);

                        if (pending.size() > 128)
                        {
                            pending.pop_front();
                        }

                        connection::send_input(input);
                    }
                }

                visual_offset = visual_offset * 0.8f;
                {
                    std::lock_guard<std::mutex> lock(data.pose_mutex);
                    data.pose = avatar.position + visual_offset;
                }

                next += tick;

                if (next < clock::now())
                {
                    next = clock::now();
                }

                std::this_thread::sleep_until(next);
            }
        }
    } // namespace

    void init()
    {
        auto& data = get();

        data.stop.store(false, std::memory_order_relaxed);
        data.buttons.store(0, std::memory_order_relaxed);
        data.yaw.store(0, std::memory_order_relaxed);
        data.ticks.store(0, std::memory_order_relaxed);
        data.error.store(0, std::memory_order_relaxed);

        data.worker = std::thread(run);
    }

    void controls(std::uint32_t buttons, float yaw)
    {
        get().buttons.store(buttons, std::memory_order_relaxed);
        get().yaw.store(yaw, std::memory_order_relaxed);
    }

    vk::util::math::vec3 position()
    {
        auto& data = get();

        std::lock_guard<std::mutex> lock(data.pose_mutex);
        return data.pose;
    }

    std::uint64_t ticks()
    {
        return get().ticks.load(std::memory_order_relaxed);
    }

    float correction()
    {
        return get().error.load(std::memory_order_relaxed);
    }

    void shutdown()
    {
        auto& data = get();

        data.stop.store(true, std::memory_order_relaxed);

        if (data.worker.joinable())
        {
            data.worker.join();
        }
    }
} // namespace clt::prediction
