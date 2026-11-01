#include "application.hpp"

namespace game::application
{
    state* get()
    {
        static state data;
        return &data;
    }
} // namespace game::application
