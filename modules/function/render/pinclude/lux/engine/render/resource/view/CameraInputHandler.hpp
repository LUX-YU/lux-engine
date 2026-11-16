#pragma once

#include <lux/engine/render/extensions/detail/FrameDispatchContext.hpp>

namespace lux::render::detail
{
    class CameraInputHandler final
    {
    public:
        static RenderResult<void> apply(FrameViewTarget, CameraInput) noexcept;
    };
} // namespace lux::render::detail
