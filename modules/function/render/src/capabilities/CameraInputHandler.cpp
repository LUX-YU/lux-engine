#include <lux/engine/render/capabilities/CameraInputHandler.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>

namespace lux::render::detail
{
    CameraInputHandler* CameraInputHandler::create(ResourceRegistry&) noexcept
    {
        return new CameraInputHandler;
    }

    RenderResult<void> CameraInputHandler::apply(FrameParameterTarget target, CameraInput input) noexcept
    {
        const RasterViewport viewport{input.viewport[0], input.viewport[1], input.viewport[2], input.viewport[3]};
        if (auto valid = viewport.validate(); !valid)
        {
            return valid;
        }
        auto applied = target.instance.setParameterInput(std::as_bytes(std::span{&input, 1}), target.projection);
        if (applied)
        {
            target.instance.setViewport(viewport);
        }
        return applied;
    }
} // namespace lux::render::detail
