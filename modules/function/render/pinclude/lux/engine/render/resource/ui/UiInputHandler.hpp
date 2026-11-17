#pragma once

#include <lux/engine/render/extensions/detail/FrameDrawTarget.hpp>
#include <lux/engine/render/inputs/ImguiFrameData.hpp>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::detail
{
    class UiInputHandler final
    {
    public:
        static RenderResult<void> draw(ResourceRegistry&, FrameDrawTarget, const ImguiFrameData&) noexcept;
    };
} // namespace lux::render::detail
