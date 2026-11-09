#pragma once

#include <lux/engine/render/extensions/detail/FrameDispatchContext.hpp>
#include <lux/engine/render/inputs/CameraInputs.hpp>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::detail
{
    class CameraInputHandler final
    {
    public:
        static constexpr std::string_view name = "lux.render.camera.input";
        static CameraInputHandler* create(ResourceRegistry&) noexcept;
        RenderResult<void> apply(FrameParameterTarget target, CameraInput input) noexcept;
    };
}
