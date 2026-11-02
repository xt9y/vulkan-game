#include "application.hpp"

#include "srv/game/application.hpp"
#include "srv/assets/model.hpp"
#include "srv/network/session.hpp"

#include "srv/shared/net/endpoint.hpp"
#include "srv/shared/net/message.hpp"
#include "srv/shared/net/animation.hpp"
#include "srv/shared/net/scene_stream.hpp"
#include "srv/shared/net/movement.hpp"
#include "srv/shared/net/socket.hpp"
#include "srv/shared/player/movement.hpp"
#include "srv/jobs/worker.hpp"
#include "srv/shared/net/limits.hpp"

#include <algorithm>
#include <iostream>
#include <filesystem>
#include <cmath>
#include <string>
#include <iterator>
#include <vector>


#include <stdexcept>
#include <thread>

namespace srv::application
{
    using clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    namespace
    {
        void receive_messages();
        void send_scene_fragments();
        void send_animation_states();
        void send_attachment_events();
        void handle(const net::endpoint::resource& peer, const net::message::resource& message);
        void reply(
            const net::endpoint::resource& peer,
            net::message::type type,
            const net::message::resource& request
        );
        void tick_players();
    } // namespace

    state* get()
    {
        static state data;
        return &data;
    }

    void set_scene(::scene::state scene)
    {
        if (get()->initialized) throw std::runtime_error("Cannot replace a running scene");
        get()->scene = std::move(scene);
        get()->scene_configured = true;
    }

    void load_model(std::filesystem::path path, ::scene::transform pose,
                    std::uint32_t animation)
    {
        if (get()->initialized) throw std::runtime_error("Register model imports before init");
        if (path.empty() || !std::filesystem::is_regular_file(path))
            throw std::runtime_error("Model file not found: " + path.string());
        get()->models.push_back({std::move(path), pose, animation});
    }

    void init()
    {
        if (get()->initialized)
        {
            throw std::runtime_error("srv::application already initialized");
        }

        if (!get()->scene_configured)
            throw std::runtime_error("The game must provide a scene with set_scene()");
        get()->options = *game::options::get();
        get()->socket = net::socket::create(get()->options.port, get()->options.bind_host);
        session::init();
        get()->colliders = player::build_colliders(get()->scene);
        get()->collision_index = player::build_index(get()->colliders);
        srv::generation::init();
        get()->scene_bytes = net::scene_stream::serialize(get()->scene);
        get()->scene_hash = net::scene_stream::checksum(get()->scene_bytes);
        get()->scene_batches = {{get()->scene_bytes, get()->scene_hash, true}};
        get()->tick = 0;
        get()->initialized = true;

        // Import game-declared assets (including textures, skins and clips).
        // Vulkan resources remain entirely client-side.
        auto requests = std::move(get()->models);
        if (!requests.empty())
        {
            begin_stream();
            if (!generation::submit([requests = std::move(requests)]() -> generation::completion
            {
                ::scene::state combined;
                auto mesh_id = std::uint32_t(1);
                auto instance_id = std::uint32_t(1);
                auto texture_id = std::uint32_t(1);
                auto skin_id = std::uint32_t(1);
                for (const auto& mesh : get()->scene.meshes)
                    mesh_id = std::max(mesh_id, mesh.id + 1);
                for (const auto& instance : get()->scene.instances)
                    instance_id = std::max(instance_id, instance.id + 1);
                for (const auto& texture : get()->scene.textures)
                    texture_id = std::max(texture_id, texture.id + 1);
                for (const auto& skin : get()->scene.skins)
                    skin_id = std::max(skin_id, skin.id + 1);

                for (const auto& request : requests)
                {
                    const auto import_started = clock::now();
                    auto imported = assets::load(request.path, mesh_id, instance_id,
                                                 texture_id, skin_id);
                    if (imported.meshes.empty())
                        throw std::runtime_error("Model has no renderable meshes: " + request.path.string());
                    for (auto& instance : imported.instances)
                    {
                        const auto mesh = std::find_if(imported.meshes.begin(), imported.meshes.end(),
                            [&](const ::scene::mesh& entry) { return entry.id == instance.mesh_id; });
                        if (mesh == imported.meshes.end())
                            throw std::runtime_error("Imported instance references a missing mesh");
                        if (mesh->skin_id)
                        {
                            const auto skin = std::find_if(imported.skins.begin(), imported.skins.end(),
                                [&](const ::scene::skin& entry) { return entry.id == mesh->skin_id; });
                            if (skin == imported.skins.end() ||
                                (request.animation >= skin->clips.size() &&
                                 !(skin->clips.empty() && request.animation == 0)))
                                throw std::runtime_error("Requested animation clip does not exist: " +
                                                         request.path.string());
                        }
                        instance.pose = request.pose;
                        instance.animation = request.animation;
                    }
                    std::cout << "srv: imported " << request.path.string() << " ("
                              << imported.meshes.size() << " meshes, "
                              << imported.instances.size() << " instances, "
                              << imported.textures.size() << " textures, "
                              << imported.skins.size() << " skins, "
                              << std::chrono::duration_cast<std::chrono::milliseconds>(
                                     clock::now() - import_started).count()
                              << " ms)\n";
                    mesh_id += std::uint32_t(imported.meshes.size());
                    instance_id += std::uint32_t(imported.instances.size());
                    texture_id += std::uint32_t(imported.textures.size());
                    skin_id += std::uint32_t(imported.skins.size());
                    combined.meshes.insert(combined.meshes.end(),
                        std::make_move_iterator(imported.meshes.begin()),
                        std::make_move_iterator(imported.meshes.end()));
                    combined.instances.insert(combined.instances.end(),
                        std::make_move_iterator(imported.instances.begin()),
                        std::make_move_iterator(imported.instances.end()));
                    combined.textures.insert(combined.textures.end(),
                        std::make_move_iterator(imported.textures.begin()),
                        std::make_move_iterator(imported.textures.end()));
                    combined.skins.insert(combined.skins.end(),
                        std::make_move_iterator(imported.skins.begin()),
                        std::make_move_iterator(imported.skins.end()));
                }
                return [imported = std::make_shared<::scene::state>(std::move(combined))]() mutable
                {
                    get()->importing = std::move(imported);
                    get()->next_texture = 0;
                    get()->next_skin = 0;
                    get()->next_mesh = 0;
                    get()->next_instance = 0;
                };
            }))
                throw std::runtime_error("Could not queue model imports");
        }
    }

    void begin_stream()
    {
        auto& data = *get();
        data.scene_bytes = net::scene_stream::serialize(data.scene);
        data.scene_hash = net::scene_stream::checksum(data.scene_bytes);
        data.scene_batches = {{data.scene_bytes, data.scene_hash, false}};
    }

    void append_scene(::scene::state patch, bool final)
    {
        auto& data = *get();
        if (!data.initialized || data.scene_batches.empty() || data.scene_batches.back().final)
        {
            throw std::runtime_error("Scene stream not accepting batches");
        }
        if (data.scene_batches.size() >= 65535)
        {
            throw std::runtime_error("Too many scene batches");
        }

        auto bytes = net::scene_stream::serialize(patch);
        if (bytes.size() > net::scene_stream::max_scene_bytes)
            throw std::runtime_error("Scene asset exceeds the network batch limit (75 MiB)");
        auto hash = net::scene_stream::checksum(bytes);

        for (auto& skin : patch.skins)
            data.scene.skins.push_back(std::move(skin));

        for (auto& texture : patch.textures)
        {
            data.scene.textures.push_back(std::move(texture));
        }

        for (auto& mesh : patch.meshes)
        {
            data.scene.meshes.push_back(std::move(mesh));
        }

        // Only the new geometry needs collision bounds and a new BVH root.
        auto added_colliders = player::build_colliders(data.scene.meshes, patch.instances);
        std::vector<spatial::bounds> boxes;
        boxes.reserve(added_colliders.size());
        for (const auto& collider : added_colliders)
        {
            boxes.push_back({collider.min, collider.max});
        }

        for (auto& instance : patch.instances)
        {
            data.scene.instances.push_back(std::move(instance));
        }
        data.colliders.insert(
            data.colliders.end(), added_colliders.begin(), added_colliders.end()
        );
        data.collision_index.append(boxes);
        data.scene_batches.push_back({std::move(bytes), hash, final});
    }

    bool play_animation(std::uint32_t instance_id, std::uint32_t clip,
                        std::uint32_t overlay, float blend, std::uint32_t mask_root)
    {
        if (!std::isfinite(blend) || blend < 0.f || blend > 1.f)
            return false;
        auto& world = get()->scene;
        const auto it = std::find_if(world.instances.begin(), world.instances.end(),
            [=](const ::scene::instance& instance) { return instance.id == instance_id; });
        if (it == world.instances.end()) return false;
        const auto mesh = std::find_if(world.meshes.begin(), world.meshes.end(),
            [&](const ::scene::mesh& entry) { return entry.id == it->mesh_id; });
        if (mesh == world.meshes.end() || !mesh->skin_id) return false;
        const auto skin = std::find_if(world.skins.begin(), world.skins.end(),
            [&](const ::scene::skin& entry) { return entry.id == mesh->skin_id; });
        if (skin == world.skins.end() || clip >= skin->clips.size() ||
            (overlay != UINT32_MAX && overlay >= skin->clips.size()) ||
            (mask_root != UINT32_MAX && mask_root >= skin->inverse_bind.size()))
            return false;

        it->animation = clip;
        it->animation_tick = get()->tick;
        it->overlay_animation = overlay;
        it->overlay_weight = blend;
        it->overlay_root = mask_root;
        return true;
    }

    bool emit_attachment_event(std::uint32_t instance_id,
                               std::uint32_t joint, std::uint32_t kind)
    {
        auto& state = *get();
        if (state.attachment_events.size() >= 128) return false;
        const auto it = std::find_if(state.scene.instances.begin(),
            state.scene.instances.end(), [&](const ::scene::instance& instance)
            { return instance.id == instance_id; });
        if (it == state.scene.instances.end()) return false;
        const auto mesh = std::find_if(state.scene.meshes.begin(),
            state.scene.meshes.end(), [&](const ::scene::mesh& m)
            { return m.id == it->mesh_id; });
        if (mesh == state.scene.meshes.end() || !mesh->skin_id) return false;
        const auto skin = std::find_if(state.scene.skins.begin(),
            state.scene.skins.end(), [&](const ::scene::skin& entry)
            { return entry.id == mesh->skin_id; });
        if (skin == state.scene.skins.end() ||
            joint >= skin->inverse_bind.size()) return false;
        state.attachment_events.push_back({
            {0, state.tick, ++state.next_event, instance_id, joint, kind}, 3
        });
        return true;
    }

    int run()
    {
        if (!get()->initialized)
        {
            throw std::runtime_error("srv::application is not initialized");
        }

        const auto started = clock::now();
        auto next = started;
        const auto interval = std::chrono::nanoseconds(1'000'000'000 / net::limits::tick_hz);

        while (!game::application::get()->stop.load(std::memory_order_relaxed))
        {
            const auto now = clock::now();

            if (get()->options.duration > 0 &&
                std::chrono::duration<double>(now - started).count() >= get()->options.duration)
            {
                break;
            }
            ++get()->tick;

            session::expire(now);

            // Process input and simulation before bulk scene data. Bound the
            // amount of network work done inside one authoritative tick.
            receive_messages();
            tick_players();
            send_animation_states();
            send_attachment_events();

            // Group small assets together instead of adding only one asset
            // every 60 Hz tick. Bound serialization work to a few MiB per batch.
            if (get()->importing)
            {
                auto& imported = *get()->importing;
                ::scene::state patch;
                constexpr std::size_t target_bytes = 4 * 1024 * 1024;
                std::size_t used = 20;
                auto fits = [&](std::size_t size)
                {
                    return used == 20 || size <= target_bytes - used;
                };

                while (true)
                {
                    std::size_t size = 0;
                    if (get()->next_texture < imported.textures.size())
                    {
                        auto& texture = imported.textures[get()->next_texture];
                        size = 20 + (texture.encoded.empty()
                            ? texture.pixels.size() : texture.encoded.size());
                        if (!fits(size)) break;
                        patch.textures.push_back(std::move(texture));
                        ++get()->next_texture;
                    }
                    else if (get()->next_skin < imported.skins.size())
                    {
                        auto& skin = imported.skins[get()->next_skin];
                        size = 16 + skin.skeleton.size() + skin.inverse_bind.size() * 64;
                        for (const auto& clip : skin.clips)
                            size += 8 + clip.name.size() + clip.bytes.size();
                        if (!fits(size)) break;
                        patch.skins.push_back(std::move(skin));
                        ++get()->next_skin;
                    }
                    else if (get()->next_mesh < imported.meshes.size())
                    {
                        auto& mesh = imported.meshes[get()->next_mesh];
                        size = 96 + mesh.vertices.size() * (mesh.skin_id ? 64 : 32) +
                            mesh.indices.size() * 4;
                        if (!fits(size)) break;
                        patch.meshes.push_back(std::move(mesh));
                        ++get()->next_mesh;
                    }
                    else if (get()->next_instance < imported.instances.size())
                    {
                        size = 80;
                        if (!fits(size)) break;
                        patch.instances.push_back(
                            std::move(imported.instances[get()->next_instance++]));
                    }
                    else break;

                    used += size;
                    if (used >= target_bytes) break;
                }

                const bool final =
                    get()->next_texture == imported.textures.size() &&
                    get()->next_skin == imported.skins.size() &&
                    get()->next_mesh == imported.meshes.size() &&
                    get()->next_instance == imported.instances.size();

                if (!patch.textures.empty() || !patch.skins.empty() ||
                    !patch.meshes.empty() || !patch.instances.empty())
                    append_scene(std::move(patch), final);

                if (final)
                {
                    std::size_t bytes = 0;
                    for (const auto& batch : get()->scene_batches)
                        bytes += batch.bytes.size();
                    std::cout << "srv: model stream ready: " << get()->scene_batches.size()
                              << " batches, " << bytes / 1024 << " KiB on the wire\n";
                    get()->importing.reset();
                }
            }

            send_scene_fragments();

            // Streaming generation may be expensive; commit only when there is
            // room before the next tick, without blocking player simulation.
            if (clock::now() +
                std::chrono::milliseconds(net::limits::generation_slack_ms) <
                next + interval ||
                get()->tick % net::limits::generation_commit_ticks == 0)
            {
                generation::commit();
            }

            next += interval;
            const auto completed = clock::now();

            // Recover up to one missed tick on wakeup jitter rather than
            // permanently shifting the 60 Hz schedule behind real time.
            if (completed > next + interval)
            {
                next = completed - interval;
            }
            if (next > completed)
            {
                std::this_thread::sleep_until(next);
            }
        }

        return 0;
    }

    void shutdown() noexcept
    {
        net::socket::destroy(get()->socket);
        session::shutdown();
        generation::shutdown();
        get()->scene_bytes.clear();
        get()->scene_batches.clear();
        get()->colliders.clear();
        get()->scene_hash = 0;
        get()->scene = {};
        get()->models.clear();
        get()->scene_configured = false;
        get()->importing.reset();
        get()->next_texture = 0;
        get()->next_skin = 0;
        get()->next_mesh = 0;
        get()->next_instance = 0;
        get()->animation_cursor = 0;
        get()->attachment_events.clear();
        get()->next_event = 0;
        get()->tick = 0;
        get()->initialized = false;
    }

    namespace
    {
        void receive_messages()
        {
            // One extra byte ensures oversized datagrams can never look like valid packets after
            // truncation.
            std::array<std::uint8_t, 25> buffer{};

            const auto deadline = clock::now() +
                std::chrono::milliseconds(net::limits::receive_ms);
            for (std::uint32_t budget = 0;
                 budget < net::limits::receive_packets && clock::now() < deadline;
                 ++budget)
            {
                net::endpoint::resource peer;
                const auto size = net::socket::receive(get()->socket, peer, buffer.data(), buffer.size());

                if (!size)
                {
                    break;
                }

                if (const auto command = net::movement::decode_input(buffer.data(), *size))
                {
                    for (auto& client : session::get()->clients)
                    {
                        if (!client.active || client.token != command->token || !net::endpoint::equal(client.endpoint, peer))
                        {
                            continue;
                        }
                        
                        const auto& incoming = command->input;
                        if (!net::message::newer(incoming.sequence, client.processed_input) ||
                            incoming.sequence - client.processed_input > 256)
                        {
                            break;
                        }

                        const auto duplicate = std::find_if(client.inputs.begin(), client.inputs.end(), [&](const player::input& value){
                            return value.sequence == incoming.sequence;
                        });

                        if (duplicate == client.inputs.end())
                        {
                            // Always admit the earliest missing input. Otherwise a full
                            // out-of-order queue could permanently block reconciliation.
                            if (client.inputs.size() >= 128)
                            {
                                if (incoming.sequence != client.processed_input + 1)
                                {
                                    break;
                                }
                                client.inputs.pop_back();
                            }

                            const auto position = std::find_if(client.inputs.begin(), client.inputs.end(), [&](const player::input& value){
                                return net::message::newer(value.sequence, incoming.sequence);
                            });

                            client.inputs.insert(position, incoming);
                        }

                        client.last_seen = clock::now();
                        break;
                    }
                    
                    continue;
                }

                if (const auto message = net::message::decode(buffer.data(), *size))
                {
                    handle(peer, *message);
                }
            }
        }

        void handle(const net::endpoint::resource& peer, const net::message::resource& message)
        {
            session::resource* existing = nullptr;
            session::resource* available = nullptr;

            for (auto& client : session::get()->clients)
            {
                if (client.active && net::endpoint::equal(client.endpoint, peer))
                {
                    existing = &client;
                }

                if (!client.active && !available)
                {
                    available = &client;
                }
            }

            if (message.type == net::message::type::hello)
            {
                auto* client = existing ? existing : available;

                if (!client || message.token == 0)
                {
                    return;
                }

                if (!client->active || client->token != message.token)
                {
                    *client = session::resource{};
                    client->endpoint = peer;
                    client->token = message.token;
                    client->active = true;
                    client->avatar.position = player::parameters().spawn;
                }

                client->last_seen = clock::now();
                reply(peer, net::message::type::welcome, message);

                return;
            }

            if (!existing || existing->token != message.token)
            {
                return;
            }

            if (message.type == net::message::type::goodbye)
            {
                existing->active = false;
            }
            else if (message.type == net::message::type::ping)
            {
                if (existing->has_sequence &&
                    !net::message::newer(message.sequence, existing->last_sequence))
                {
                    return;
                }

                existing->last_sequence = message.sequence;
                existing->has_sequence = true;
                existing->last_seen = clock::now();
                reply(peer, net::message::type::state, message);
            }
            else if (message.type == net::message::type::scene_request)
            {
                existing->last_seen = clock::now();
                // One pending request per session; never spend physics time
                // transmitting bulk scene packets from the receive loop.
                existing->scene_request = message.value;
                existing->scene_pending = true;
            }
        }

        void send_attachment_events()
        {
            auto& queue = get()->attachment_events;
            for (auto& item : queue)
            {
                for (const auto& client : session::get()->clients)
                {
                    if (!client.active) continue;
                    auto event = item.event;
                    event.token = client.token;
                    const auto bytes = net::animation::encode_event(event);
                    net::socket::send(get()->socket, client.endpoint,
                                      bytes.data(), bytes.size());
                }
                if (item.repeats) --item.repeats;
            }
            queue.erase(std::remove_if(queue.begin(), queue.end(),
                [](const state::pending_event& item) { return !item.repeats; }),
                queue.end());
        }

        void send_animation_states()
        {
            if (get()->tick % net::limits::animation_period != 0 || get()->scene.instances.empty()) return;
            const auto& instances = get()->scene.instances;
            const std::size_t count = std::min<std::size_t>(net::limits::animations_per_tick, instances.size());
            const std::size_t start = get()->animation_cursor % instances.size();
            for (auto& client : session::get()->clients)
            {
                if (!client.active) continue;
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto& instance = instances[(start + i) % instances.size()];
                    const auto mesh = std::find_if(
                        get()->scene.meshes.begin(), get()->scene.meshes.end(),
                        [&](const ::scene::mesh& m) { return m.id == instance.mesh_id; });
                    if (mesh == get()->scene.meshes.end() || !mesh->skin_id)
                        continue;
                    const auto message = net::animation::encode({
                        client.token, get()->tick, instance.id, instance.animation,
                        instance.animation_tick, instance.overlay_animation,
                        instance.overlay_root, instance.overlay_weight
                    });
                    net::socket::send(
                        get()->socket, client.endpoint, message.data(), message.size());
                }
            }
            get()->animation_cursor = (start + count) % instances.size();
        }

        void send_scene_fragments()
        {
            const auto deadline = clock::now() +
                std::chrono::milliseconds(net::limits::fragments_ms);
            std::uint32_t remaining = net::limits::fragments_per_tick;

            for (auto& client : session::get()->clients)
            {
                if (!client.active || !client.scene_pending || !remaining ||
                    clock::now() >= deadline)
                {
                    continue;
                }
                client.scene_pending = false;

                const auto batch_id = client.scene_request >> 16;
                const auto fragment_id = client.scene_request & 0xffffu;
                if (batch_id >= get()->scene_batches.size())
                {
                    continue;
                }

                const auto& batch = get()->scene_batches[batch_id];
                const auto count = (batch.bytes.size() +
                    net::scene_stream::fragment_size - 1) /
                    net::scene_stream::fragment_size;

                for (std::size_t i = fragment_id;
                     i < std::min<std::size_t>(count, fragment_id + net::limits::fragments_per_tick) &&
                     remaining && clock::now() < deadline; ++i)
                {
                    net::scene_stream::packet packet;
                    const auto size = net::scene_stream::encode_fragment(
                        batch.bytes, client.token, static_cast<std::uint32_t>(i),
                        batch.hash, packet, batch.final
                    );
                    if (!size)
                    {
                        break;
                    }

                    const auto index = (batch_id << 16) |
                        static_cast<std::uint32_t>(i);
                    packet[16] = std::uint8_t(index >> 24);
                    packet[17] = std::uint8_t(index >> 16);
                    packet[18] = std::uint8_t(index >> 8);
                    packet[19] = std::uint8_t(index);

                    if (!net::socket::send(
                        get()->socket, client.endpoint, packet.data(), size))
                    {
                        break; // The client retries from its missing index.
                    }
                    --remaining;
                }
            }
        }

        void reply(const net::endpoint::resource& peer, net::message::type type, const net::message::resource& request)
        {
            const auto data =
                net::message::encode({type, request.sequence, request.token, get()->tick});
            net::socket::send(get()->socket, peer, data.data(), data.size());
            const auto settings = net::movement::encode_parameters(
                request.token, player::parameters());
            net::socket::send(get()->socket, peer, settings.data(), settings.size());
        }

        void tick_players()
        {
            for (auto& client : session::get()->clients)
            {
                if (!client.active)
                {
                    continue;
                }

                // Apply contiguous inputs only. Never simulate a phantom input tick.
                for (std::uint32_t budget = 0; budget < net::limits::inputs_per_tick && !client.inputs.empty(); ++budget)
                {
                    if (client.inputs.front().sequence != client.processed_input + 1)
                    {
                        break;
                    }

                    const auto input = client.inputs.front();
                    client.inputs.pop_front();
                    player::step(client.avatar, input, get()->colliders, &get()->collision_index);
                    client.processed_input = input.sequence;
                }

                net::movement::snapshot snapshot;
                snapshot.token = client.token;
                snapshot.sequence = client.processed_input;
                snapshot.tick = get()->tick;
                snapshot.player = client.avatar;

                const auto packet = net::movement::encode(snapshot);

                net::socket::send(get()->socket, client.endpoint, packet.data(), packet.size());
            }
        }
    } // namespace
} // namespace srv::application
