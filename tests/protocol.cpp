#define STB_IMAGE_IMPLEMENTATION
#include "server/net/animation.hpp"
#include "server/net/editor_cube.hpp"
#include "server/net/movement.hpp"
#include "server/net/weapon.hpp"
#include "server/net/scene_stream.hpp"
#include "server/srv/session.hpp"
#include <cassert>
#include <iostream>
#include <random>

// This fingerprint covers exact bytes and decoder acceptance, including mutations.
// It was captured before replacing the duplicated field readers/writers.
std::uint64_t fingerprint = 14695981039346656037ull;
void record(std::uint8_t byte) { fingerprint = (fingerprint ^ byte) * 1099511628211ull; }
template<class Bytes, class Decode>
void exercise(const Bytes& bytes, Decode decode)
{
    for (auto byte : bytes) record(byte);
    assert(decode(bytes.data(), bytes.size()));
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        assert(!decode(bytes.data(), n));
        record(0);
    }
    for (std::size_t i = 0; i < bytes.size(); ++i)
        for (auto mask : {1, 127, 255}) {
            auto corrupt = bytes;
            corrupt[i] ^= mask;
            record(bool(decode(corrupt.data(), corrupt.size())));
        }
}
int main()
{
    using namespace net;
    for (std::uint32_t i = 1; i <= 32; ++i) {
        exercise(message::encode({message::type(i % 8 + 1), i, i + 5, i * 9}), message::decode);
        exercise(animation::encode({i, i * 8, i + 1, i % 3, i, UINT32_MAX, UINT32_MAX, .25f}), animation::decode);
        exercise(movement::encode(i, {i, i & 31, float(i) / 7}), movement::decode_input);
        exercise(movement::encode_parameters(i, {{1, -2, 3}, 4, 14, 20, .3f, 1.8f}), movement::decode_parameters);
        movement::snapshot s{i, i * 2, i * 3, {{1, -2, float(i)}, {.3f, -7, 0}, bool(i % 2)}};
        exercise(movement::encode(s), movement::decode_snapshot);
        exercise(movement::encode_peers({i, i + 4, {{0, {1, 2, 3}}, {2, {-4, 5, 6}}}}), movement::decode_peers);
        editor_light::resource l{editor_light::kind::state, i, i + 1, i % 64, i, {{1, 2, -3}, 4, {.1f, .5f, 1}, 8, bool(i % 2)}};
        exercise(editor_light::encode(l), editor_light::decode);
        editor_cube::resource c{};
        c.token = i; c.sequence = i + 1; c.index = i; c.revision = i;
        c.cube.mesh_id = ::scene::unit_cube_mesh_id;
        c.cube.pose.position = {1, -2, 3}; c.cube.tint = {.1f, .5f, 1};
        exercise(editor_cube::encode(c), editor_cube::decode);
        net::weapon::command command{i, i + 1, ::weapon::action::fire, .25f, -.3f, 1};
        exercise(net::weapon::encode_command(command), net::weapon::decode_command);
        net::weapon::snapshot w{i, i + 1, i + 2, 12, 100, ::weapon::reload_phase::none, ::weapon::result::fired, 0, 0, 0, 1, 1};
        exercise(net::weapon::encode_snapshot(w), net::weapon::decode_snapshot);
        exercise(net::weapon::encode_shot({w, {true, {1, 2, 3}, {0, 1, 0}, 1, 2}}), net::weapon::decode_shot);
    }
    ::scene::state world;
    world.textures.push_back({1, 1, 1, {20, 40, 60, 255}, {}});
    world.skins.push_back({1, {1, 2, 3}, {{"walk", {4, 5, 6}}}, {std::array<float, 16>{}}});
    ::scene::mesh mesh{};
    mesh.id = 1; mesh.skin_id = 1;
    mesh.vertices = {{{0, 0, 0}, {0, 1, 0}, 0, 0, {0, 0, 0, 0}, {1, 0, 0, 0}},
                     {{1, 0, 0}, {0, 1, 0}, 1, 0, {}, {1, 0, 0, 0}},
                     {{0, 0, 1}, {0, 1, 0}, 0, 1, {}, {1, 0, 0, 0}}};
    mesh.indices = {0, 1, 2}; mesh.surface.double_sided = true;
    world.meshes.push_back(mesh);
    mesh.id = 2; mesh.skin_id = 0; world.meshes.push_back(mesh);
    world.instances.push_back({1, 1});
    world.lights.push_back({{0, 3, 1}, 6, {1, .5f, .2f}, 4});
    world.collision_boxes.push_back({{-1, -2, -3}, {1, 2, 3}});
    auto bytes = scene_stream::serialize(world);
    ::scene::state restored;
    assert(scene_stream::deserialize(bytes, restored));
    assert(scene_stream::serialize(restored) == bytes);
    for (auto b : bytes) record(b);
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        auto truncated = bytes; truncated.resize(n);
        assert(!scene_stream::deserialize(truncated, restored));
    }
    for (std::size_t i = 0; i < bytes.size(); ++i)
        for (auto mask : {1, 127, 255}) {
            auto corrupt = bytes; corrupt[i] ^= mask;
            record(scene_stream::deserialize(corrupt, restored));
        }
    auto batch = scene_stream::encode_batch(bytes, 17, 2, true);
    scene_stream::batch_payload decoded;
    assert(scene_stream::decode_batch(batch, decoded));
    assert(decoded.scene == bytes && decoded.final && decoded.token == 17 && decoded.batch == 2);
    for (std::size_t i = 0; i < batch.size(); ++i) {
        auto corrupt = batch; corrupt[i] ^= 255;
        record(scene_stream::decode_batch(corrupt, decoded));
    }
    assert(message::newer(1, UINT32_MAX));
    assert(!message::newer(UINT32_MAX, 1));
    assert(!message::newer(0x80000000u, 0));
    // Authentication must bind the token to an active session and exact endpoint.
    srv::session::init();
    auto& client = srv::session::get()->clients.front();
    client.endpoint = endpoint::resolve("127.0.0.1", 27015);
    client.token = 42; client.active = true;
    assert(srv::session::authenticated(client.endpoint, 42) == &client);
    assert(!srv::session::authenticated(client.endpoint, 43));
    assert(!srv::session::authenticated(endpoint::resolve("127.0.0.1", 27016), 42));
    assert(!srv::session::authenticated(endpoint::resolve("127.0.0.2", 27015), 42));
    client.active = false;
    assert(!srv::session::authenticated(client.endpoint, 42));
    srv::session::shutdown();

    ::scene::transform pose;
    pose.position = {10, 20, 30}; pose.scale = {2, 3, -4};
    auto position = ::scene::transform_position(pose, {1, 2, 3});
    assert(position.x == 12 && position.y == 26 && position.z == 18);
    pose.rotation = vk::util::math::euler(0, 1.5707963267948966f, 0);
    position = ::scene::transform_position(pose, {1, 0, 0});
    assert(std::abs(position.x - 10) < .0001f && std::abs(position.z - 28) < .0001f);
    auto duplicate = world.meshes.front(); duplicate.vertices.clear();
    std::vector<::scene::mesh> definitions{world.meshes.front(), duplicate};
    std::vector<::scene::instance> instances{{1, 1}, {2, 99}, {3, 1}};
    const auto colliders = ::player::build_colliders(definitions, instances);
    assert(colliders.size() == 2 && colliders.front().max.x == 1 && colliders.front().max.z == 1);

    ::scene::state encoded_world;
    encoded_world.textures.push_back({1, 1, 1, {}, {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0, 31, 21, 196, 137, 0, 0, 0, 13, 73, 68, 65, 84, 120, 156, 99, 16, 209, 176, 249, 15, 0, 2, 68, 1, 120, 106, 40, 52, 133, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130}});
    assert(scene_stream::deserialize(scene_stream::serialize(encoded_world), restored));
    assert(restored.textures.front().pixels == std::vector<std::uint8_t>({20, 40, 60, 255}));
    encoded_world.textures.front().width = 2;
    assert(!scene_stream::deserialize(scene_stream::serialize(encoded_world), restored));
    const auto unchanged = scene_stream::serialize(restored);
    auto trailing = bytes; trailing.push_back(0);
    assert(!scene_stream::deserialize(trailing, restored));
    assert(scene_stream::serialize(restored) == unchanged);
    world.lights.front().strength = std::numeric_limits<float>::infinity();
    bool rejected = false;
    try { scene_stream::serialize(world); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    assert(fingerprint == 0xe6b9287d7c634fb9ull);
    std::cout << std::hex << fingerprint << '\n';
}
