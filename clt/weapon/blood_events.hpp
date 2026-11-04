#pragma once
#include "srv/shared/weapon/weapon.hpp"

#include <memory>

namespace clt::blood_events
{
    // Scene, hit geometry and networking belong to the game. All liquid
    // simulation, material response and stain accumulation belong to bld/fld.
    void shot(const ::weapon::hit& hit,
              const std::shared_ptr<const ::scene::state>& scene);
    void update(float seconds,const ::weapon::world& collision,
                const std::shared_ptr<const ::scene::state>& scene);
    void reset();
}
