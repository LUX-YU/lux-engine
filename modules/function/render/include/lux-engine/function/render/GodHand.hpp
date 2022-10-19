#pragma once
#include "lux-engine/resource/model/Model.hpp"

namespace lux::engine::function
{
    class GodHand
    {
    public:
        LUX_EXPORT GodHand();

        LUX_EXPORT void drawModel(lux::engine::resource::Model);
    };
} // namespace lux::engine::function
