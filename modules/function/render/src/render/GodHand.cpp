#include "lux-engine/function/render/GodHand.hpp"
#include "GodHandOpenGLImpl.hpp"

namespace lux::engine::function
{
    GodHand::GodHand(LuxWindow& window)
    {
        using API_TYPES = ::lux::engine::platform::GraphicAPI;
        if(window.graphicAPIType() == API_TYPES::OPENGL)
        {
            _impl = std::make_unique<GodHandOpenGLImpl>();
        }
    }

    GodHand::~GodHand() = default;

    void GodHand::drawModel(const LuxModel& model, const Position& position)
    {
        _impl->drawModel(model, position);
    }
}
