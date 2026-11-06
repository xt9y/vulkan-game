#pragma once
#include "srv/shared/scene/scene.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace net::scene_stream {
inline constexpr std::size_t max_scene_bytes=75*1024*1024;
// Application-owned envelope for one complete scene batch. Fragmentation,
// retransmission and congestion control are implemented by net::session.
struct batch_payload {
    std::uint32_t token=0;
    std::uint32_t batch=0;
    bool final=false;
    std::vector<std::uint8_t> scene;
};
std::vector<std::uint8_t> serialize(const ::scene::state& scene);
bool deserialize(const std::vector<std::uint8_t>& bytes,::scene::state& scene);
std::uint32_t checksum(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> encode_batch(const std::vector<std::uint8_t>& bytes,
    std::uint32_t token,std::uint32_t batch,bool final);
bool decode_batch(const std::vector<std::uint8_t>& bytes,batch_payload& result);
}
