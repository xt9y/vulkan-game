#pragma once

#include "srv/shared/weapon/gun.hpp"
#include <vector>

// Game-owned gun catalog. Both server and client consume the same complete
// profiles: the server reads authoritative rules, the client loads models,
// animation clips, controls, ADS behavior, recoil and effects.
namespace game_weapons
{
    std::vector<::weapon::gun> all();
}
