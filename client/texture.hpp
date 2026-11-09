#pragma once
#include "../server/scene.hpp"
#include <vk/vk.hpp>

namespace clt::texture {
    void init();
    void synchronize(const ::scene::state& source);
    void bind(vk::DescriptorSet set, const ::scene::material& surface);
    void pin(const ::scene::state& source);
    void reset();
    void shutdown();

} // namespace clt::texture
