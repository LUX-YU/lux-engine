#pragma once
#include <lux-engine/function/render/GodHand.hpp>

namespace lux::engine::function
{
    class GodHandInterface
    {
    public:
        virtual void drawModel(const LuxModel& model, const Position& position) = 0;
    };
}
