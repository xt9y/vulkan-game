#include "application.hpp"

#include "srv/assets/model.hpp"
#include "srv/game/application.hpp"
#include <jobs/pool.hpp>
#include "srv/network/session.hpp"
#include "srv/shared/net/animation.hpp"
#include "srv/shared/net/editor_light.hpp"
#include <network/endpoint.hpp>
#include "srv/shared/net/limits.hpp"
#include "srv/shared/net/message.hpp"
#include "srv/shared/net/movement.hpp"
#include "srv/shared/net/scene_stream.hpp"
#include <network/secure.hpp>
#include "srv/shared/net/secure_config.hpp"
#include <network/session.hpp>
#include "srv/shared/net/weapon.hpp"
#include "srv/shared/player/movement.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace srv::application
{
    using clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    namespace
    {
        jobs::pool& generation_jobs()
        {
            static jobs::pool worker;
            return worker;
        }

        void receive_messages();
        void send_editor_lights();
        void handle_editor_light(const net::endpoint::resource&,const net::editor_light::resource&);
        void send_scene_batches();
        void send_animation_states();
        void send_attachment_events();
        void handle(const net::endpoint::resource& peer, const net::message::resource& message);
        void reply(
            const net::endpoint::resource& peer,
            net::message::type type,
            const net::message::resource& request
        );
        void tick_players();
        void tick_weapons();
        void
        handle_weapon(const net::endpoint::resource& peer, const net::weapon::command& command);
        net::weapon::snapshot weapon_snapshot(
            const session::resource& client,
            const session::resource::owned_weapon& owned,
            double now
        );
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
        get()->editor_light_versions.assign(get()->scene.lights.size(),1u);
        get()->editor_light_revision=1;
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

    void add_weapon(::weapon::definition definition)
    {
        if (get()->initialized)
        {
            throw std::runtime_error("Register weapons before init");
        }
        get()->weapons.add(std::move(definition));
    }

    void on_weapon_command(weapon_command_handler handler)
    {
        if (get()->initialized)
        {
            throw std::runtime_error("Configure weapon handler before init");
        }
        get()->on_weapon_command = std::move(handler);
    }

    void on_editor_permission(
        std::function<bool(const net::endpoint::resource&)> handler)
    {
        if(get()->initialized)
            throw std::runtime_error("Configure editor permission before init");
        get()->editor_permission=std::move(handler);
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
        get()->socket = net::secure::create(
            net::secure::role::server,
            game::secure_config::load(net::secure::role::server),
            get()->options.port,get()->options.bind_host);
        net::session::settings transport_settings;
        transport_settings.channel.max_message=net::scene_stream::max_scene_bytes+20;
        transport_settings.channel.max_queued=2*(net::scene_stream::max_scene_bytes+20);
        transport_settings.channel.window=512;
        transport_settings.max_peers=net::limits::max_clients;
        transport_settings.allow_unknown_peers=false;
        get()->scene_transport=std::make_unique<net::session>(transport_settings);
        session::init();
        get()->colliders = player::build_colliders(get()->scene);
        get()->collision_index = player::build_index(get()->colliders);
        get()->weapon_world.append(get()->scene.meshes, get()->scene.instances);
        generation_jobs().start(1,64);
        get()->scene_batches = {{net::scene_stream::serialize(get()->scene),true}};
        get()->tick = 0;
        get()->initialized = true;

        // Import game-declared assets (including textures, skins and clips).
        // Vulkan resources remain entirely client-side.
        auto requests = std::move(get()->models);
        if (!requests.empty())
        {
            begin_stream();
            if (!generation_jobs().submit([requests = std::move(requests)]() -> jobs::completion
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
        data.scene_batches = {{net::scene_stream::serialize(data.scene),false}};
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
        data.weapon_world.append(data.scene.meshes, patch.instances);
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
        for (auto& box : patch.collision_boxes)
        {
            data.scene.collision_boxes.push_back(std::move(box));
        }
        for(auto& light:patch.lights)
        {
            data.scene.lights.push_back(std::move(light));
            data.editor_light_versions.push_back(data.editor_light_revision);
        }
        data.colliders.insert(
            data.colliders.end(), added_colliders.begin(), added_colliders.end()
        );
        data.collision_index.append(boxes);
        data.scene_batches.push_back({std::move(bytes),final});
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
            tick_weapons();
            send_editor_lights();
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

            send_scene_batches();
            get()->scene_transport->pump(
                [&](const net::endpoint::resource& peer,const void* bytes,std::size_t size) {
                    return net::secure::send(get()->socket,peer,bytes,size);
                },512*1200);

            // Streaming generation may be expensive; commit only when there is
            // room before the next tick, without blocking player simulation.
            if (clock::now() +
                std::chrono::milliseconds(net::limits::generation_slack_ms) <
                next + interval ||
                get()->tick % net::limits::generation_commit_ticks == 0)
            {
                generation_jobs().commit(2);
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
        get()->scene_transport.reset();
        net::secure::destroy(get()->socket);
        session::shutdown();
        generation_jobs().stop();
        get()->scene_batches.clear();
        get()->colliders.clear();
        get()->weapon_world = {};
        get()->weapons = {};
        get()->on_weapon_command = {};
        get()->editor_permission = {};
        get()->scene = {};
        get()->editor_light_versions.clear();
        get()->editor_light_revision=1;
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
        void handle_editor_light(
            const net::endpoint::resource& peer,
            const net::editor_light::resource& command)
        {
            // Authenticated DTLS is not sufficient to grant editing rights.
            // The host game must explicitly authorize this peer.
            if(!get()->editor_permission || !get()->editor_permission(peer))
                return;
            // Same DTLS endpoint/token as movement and weapons.
            for(auto& client:session::get()->clients)
            {
                if(!client.active || client.token!=command.token ||
                   !net::endpoint::equal(client.endpoint,peer))
                    continue;

                const bool duplicate=command.sequence==client.editor_light_sequence;
                if(!duplicate && (!net::message::newer(
                    command.sequence,client.editor_light_sequence) ||
                    (client.editor_light_sequence &&
                     command.sequence-client.editor_light_sequence>256u)))
                    return;

                if(!duplicate)
                {
                    std::uint32_t index=command.index;
                    if(command.type==net::editor_light::kind::create)
                    {
                        if(get()->scene.lights.size()>=64)
                            return;
                        index=static_cast<std::uint32_t>(
                            get()->scene.lights.size());
                        get()->scene.lights.push_back(command.light);
                        get()->editor_light_versions.push_back(
                            ++get()->editor_light_revision);
                    }
                    else if(command.type==net::editor_light::kind::edit)
                    {
                        if(index>=get()->scene.lights.size() ||
                           command.revision>7)return;
                        // Apply only the edited field to avoid reverting other
                        // properties with a stale client-side light copy.
                        auto& light=get()->scene.lights[index];
                        switch(command.revision)
                        {
                        case 0:light.position.x=command.light.position.x;break;
                        case 1:light.position.y=command.light.position.y;break;
                        case 2:light.position.z=command.light.position.z;break;
                        case 3:light.color.x=command.light.color.x;break;
                        case 4:light.color.y=command.light.color.y;break;
                        case 5:light.color.z=command.light.color.z;break;
                        case 6:light.radius=command.light.radius;break;
                        case 7:light.strength=command.light.strength;break;
                        }
                        get()->editor_light_versions[index]=
                            ++get()->editor_light_revision;
                    }
                    else return;
                    client.editor_light_sequence=command.sequence;
                    client.editor_light_index=index;

                    // Publish immediately to every connected viewport.
                    for(const auto& recipient:session::get()->clients)
                    {
                        if(!recipient.active)continue;
                        net::editor_light::resource changed{};
                        changed.type=net::editor_light::kind::state;
                        changed.token=recipient.token;
                        changed.index=index;
                        changed.sequence=get()->editor_light_versions[index];
                        changed.revision=changed.sequence;
                        changed.light=get()->scene.lights[index];
                        const auto bytes=net::editor_light::encode(changed);
                        net::secure::send(get()->socket,recipient.endpoint,
                                          bytes.data(),bytes.size());
                    }
                    std::clog<<"srv: editor light "<<index
                             <<(command.type==net::editor_light::kind::create?
                                " placed":" updated")<<"\n";
                }
                if(client.editor_light_index>=get()->scene.lights.size())
                    return;
                // Command ACK makes creates idempotent under retransmission.
                net::editor_light::resource ack{};
                ack.type=net::editor_light::kind::ack;
                ack.token=client.token;
                ack.sequence=client.editor_light_sequence;
                ack.index=client.editor_light_index;
                ack.revision=get()->editor_light_versions[ack.index];
                ack.light=get()->scene.lights[ack.index];
                const auto bytes=net::editor_light::encode(ack);
                net::secure::send(get()->socket,peer,bytes.data(),bytes.size());
                client.last_seen=clock::now();
                return;
            }
        }

        void send_editor_lights()
        {
            // A small periodic authoritative refresh recovers lost UDP
            // notifications and brings late joiners up to date. No meshes
            // or scene data are reserialized.
            if(get()->tick%60u!=0u)return;
            for(const auto& client:session::get()->clients)
            {
                if(!client.active)continue;
                for(std::size_t i=0;i<get()->scene.lights.size();++i)
                {
                    net::editor_light::resource update{};
                    update.type=net::editor_light::kind::state;
                    update.token=client.token;
                    update.index=static_cast<std::uint32_t>(i);
                    update.sequence=i<get()->editor_light_versions.size() ?
                        get()->editor_light_versions[i]:1;
                    update.revision=update.sequence;
                    update.light=get()->scene.lights[i];
                    const auto bytes=net::editor_light::encode(update);
                    net::secure::send(get()->socket,client.endpoint,
                                      bytes.data(),bytes.size());
                }
            }
        }

        void receive_messages()
        {
            // One extra byte ensures oversized datagrams can never look like valid packets after
            // truncation.
            std::array<std::uint8_t,1473> buffer{};

            const auto deadline = clock::now() +
                std::chrono::milliseconds(net::limits::receive_ms);
            for (std::uint32_t budget = 0;
                 budget < net::limits::receive_packets && clock::now() < deadline;
                 ++budget)
            {
                net::endpoint::resource peer;
                const auto size = net::secure::receive(get()->socket, peer, buffer.data(), buffer.size());

                if (!size)
                {
                    break;
                }

                if(get()->scene_transport->accept(peer,buffer.data(),*size,clock::now()))
                    continue;
                if(const auto editor_command=net::editor_light::decode(buffer.data(),*size))
                {
                    if(editor_command->type==net::editor_light::kind::create ||
                       editor_command->type==net::editor_light::kind::edit)
                        handle_editor_light(peer,*editor_command);
                    continue;
                }
                if (const auto command = net::weapon::decode_command(buffer.data(), *size))
                {
                    handle_weapon(peer, *command);
                    continue;
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

        net::weapon::snapshot weapon_snapshot(
            const session::resource& client,
            const session::resource::owned_weapon& owned,
            double now
        )
        {
            const auto& value = owned.state;
            net::weapon::snapshot snapshot;
            snapshot.token = client.token;
            snapshot.sequence = client.weapon_sequence;
            snapshot.weapon_id = owned.id;
            snapshot.active_weapon_id = client.active_weapon;
            snapshot.tick = get()->tick;
            snapshot.magazine = value.magazine;
            snapshot.reserve = value.reserve;
            snapshot.reload = value.reload;
            snapshot.outcome = value.last_result;
            if (value.reload != ::weapon::reload_phase::none)
            {
                snapshot.reload_duration = float(value.reload_finishes - value.reload_started);
                snapshot.reload_elapsed =
                    std::clamp(float(now - value.reload_started), 0.0f, snapshot.reload_duration);
                snapshot.refill_time = float(value.refill_at - value.reload_started);
            }
            return snapshot;
        }

        void handle_weapon(const net::endpoint::resource& peer, const net::weapon::command& command)
        {
            for (auto& client : session::get()->clients)
            {
                if (!client.active || client.token != command.token ||
                    !net::endpoint::equal(client.endpoint, peer))
                {
                    continue;
                }
                const auto now = clock::now();
                client.last_seen = now;
                if (!net::message::newer(command.sequence, client.weapon_sequence))
                {
                    if (client.last_shot && client.last_shot->state.sequence == command.sequence &&
                        client.last_shot->state.weapon_id == command.weapon_id)
                    {
                        const auto packet = net::weapon::encode_shot(*client.last_shot);
                        net::secure::send(
                            get()->socket, client.endpoint, packet.data(), packet.size()
                        );
                    }
                    return;
                }
                // Retry queues use one shared sequence across equipment changes. Hold
                // later datagrams until missing commands arrive rather than acknowledging
                // past a dropped equip/fire and permanently discarding it on the client.
                if (command.sequence - client.weapon_sequence > 256)
                {
                    return;
                }
                const auto duplicate = std::find_if(
                    client.weapon_commands.begin(),
                    client.weapon_commands.end(),
                    [&](const net::weapon::command& value)
                    {
                        return value.sequence == command.sequence;
                    }
                );
                if (duplicate == client.weapon_commands.end())
                {
                    auto expected = client.weapon_sequence + 1;
                    if (!expected)
                    {
                        ++expected;
                    }
                    if (client.weapon_commands.size() >= 64)
                    {
                        if (command.sequence != expected)
                        {
                            return;
                        }
                        client.weapon_commands.pop_back();
                    }
                    const auto position = std::find_if(
                        client.weapon_commands.begin(),
                        client.weapon_commands.end(),
                        [&](const net::weapon::command& value)
                        {
                            return net::message::newer(value.sequence, command.sequence);
                        }
                    );
                    client.weapon_commands.insert(position, command);
                }
                const double seconds =
                    std::chrono::duration<double>(now.time_since_epoch()).count();
                while (!client.weapon_commands.empty())
                {
                    auto expected = client.weapon_sequence + 1;
                    if (!expected)
                    {
                        ++expected;
                    }
                    if (client.weapon_commands.front().sequence != expected)
                    {
                        break;
                    }
                    const auto next = client.weapon_commands.front();
                    client.weapon_commands.pop_front();
                    client.weapon_sequence = next.sequence;
                    // Outcomes describe this global acknowledgement. A different
                    // item must not retain a prior command's fired/reload outcome.
                    for (auto& item : client.weapons)
                    {
                        item.state.last_result = ::weapon::result::none;
                    }
                    const auto* definition = get()->weapons.find(next.weapon_id);
                    auto owned = std::find_if(
                        client.weapons.begin(),
                        client.weapons.end(),
                        [&](const session::resource::owned_weapon& value)
                        {
                            return value.id == next.weapon_id;
                        }
                    );
                    if (!definition || owned == client.weapons.end() ||
                        (next.kind != ::weapon::action::equip &&
                         client.active_weapon != next.weapon_id))
                    {
                        continue;
                    }
                    if (next.kind == ::weapon::action::equip)
                    {
                        client.active_weapon = next.weapon_id;
                        owned->state.sequence = next.sequence;
                        owned->state.last_result = ::weapon::result::equipped;
                    }
                    else if (get()->on_weapon_command)
                    {
                        weapon_command_context context{
                            next,
                            *definition,
                            owned->state,
                            client.avatar,
                            seconds,
                            get()->weapon_world
                        };
                        const auto impact = get()->on_weapon_command(context);
                        if (next.kind == ::weapon::action::fire)
                        {
                            net::weapon::shot shot{
                                weapon_snapshot(client, *owned, seconds), impact
                            };
                            client.last_shot = shot;
                            if (client.shots.size() >= 32)
                            {
                                client.shots.pop_front();
                            }
                            client.shots.push_back({shot, 3});
                        }
                    }
                }
                return;
            }
        }

        void tick_weapons()
        {
            const double now =
                std::chrono::duration<double>(clock::now().time_since_epoch()).count();
            for (auto& client : session::get()->clients)
            {
                if (!client.active)
                {
                    continue;
                }
                for (auto& owned : client.weapons)
                {
                    const auto* definition = get()->weapons.find(owned.id);
                    if (!definition)
                    {
                        continue;
                    }
                    ::weapon::advance(owned.state, now, *definition);
                    // Publish every active state and changes to inactive ammunition,
                    // reload phase or acknowledgement. Clients can reconcile each item
                    // independently without treating prediction as accepted authority.
                    const auto update = weapon_snapshot(client, owned, now);
                    const auto& old = owned.published;
                    const bool changed =
                        !old || old->sequence != update.sequence ||
                        old->active_weapon_id != update.active_weapon_id ||
                        old->magazine != update.magazine || old->reserve != update.reserve ||
                        old->reload != update.reload || old->outcome != update.outcome;
                    const bool refresh = old && get()->tick - old->tick >= 30;
                    if (owned.id != client.active_weapon && !changed && !refresh)
                    {
                        continue;
                    }
                    const auto snapshot = net::weapon::encode_snapshot(update);
                    net::secure::send(
                        get()->socket, client.endpoint, snapshot.data(), snapshot.size()
                    );
                    owned.published = update;
                }
                for (auto& pending : client.shots)
                {
                    const auto packet = net::weapon::encode_shot(pending.value);
                    net::secure::send(get()->socket, client.endpoint, packet.data(), packet.size());
                    --pending.repeats;
                }
                std::erase_if(
                    client.shots,
                    [](const session::resource::pending_shot& pending)
                    {
                        return !pending.repeats;
                    }
                );
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
                    get()->scene_transport->close(peer);
                    *client = session::resource{};
                    client->endpoint = peer;
                    client->token = message.token;
                    client->active = true;
                    client->avatar.position = player::parameters().spawn;
                    for (const auto& definition : get()->weapons.all())
                    {
                        client->weapons.push_back(
                            {definition.id, ::weapon::make_state(definition), std::nullopt}
                        );
                    }
                    if (!client->weapons.empty())
                    {
                        client->active_weapon = client->weapons.front().id;
                    }
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
                get()->scene_transport->close(peer);
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
                // Only schedule a batch; the shared reliable channel handles delivery.
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
                    net::secure::send(get()->socket, client.endpoint,
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
                    net::secure::send(
                        get()->socket, client.endpoint, message.data(), message.size());
                }
            }
            get()->animation_cursor = (start + count) % instances.size();
        }

        void send_scene_batches()
        {
            // The application selects batches and validates its session tokens.
            // The network module alone manages fragmentation, pacing and acks.
            const auto now=clock::now();
            for(auto& client:session::get()->clients)
            {
                if(!client.active||!client.scene_pending)continue;
                client.scene_pending=false;
                const auto batch_id=client.scene_request>>16;
                if(batch_id>=get()->scene_batches.size())continue;
                if(client.queued_scene_batch==batch_id &&
                   (get()->scene_transport->pending(client.endpoint)>0 ||
                    now-client.queued_scene_at<2s))
                    continue;
                const auto& batch=get()->scene_batches[batch_id];
                auto payload=net::scene_stream::encode_batch(
                    batch.bytes,client.token,batch_id,batch.final);
                if(get()->scene_transport->send(client.endpoint,std::move(payload)))
                {
                    client.queued_scene_batch=batch_id;
                    client.queued_scene_at=now;
                }
                else client.scene_pending=true;
            }
        }

        void reply(const net::endpoint::resource& peer, net::message::type type, const net::message::resource& request)
        {
            const auto data =
                net::message::encode({type, request.sequence, request.token, get()->tick});
            net::secure::send(get()->socket, peer, data.data(), data.size());
            const auto settings = net::movement::encode_parameters(
                request.token, player::parameters());
            net::secure::send(get()->socket, peer, settings.data(), settings.size());
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

                net::secure::send(get()->socket, client.endpoint, packet.data(), packet.size());
            }
        }
    } // namespace
} // namespace srv::application
