#pragma once

#include <lux/engine/render/extensions/detail/FrameDispatchContext.hpp>
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
        static constexpr std::string_view name = "lux.render.ui.input";
        static UiInputHandler* create(ResourceRegistry&) noexcept;
        RenderResult<void> draw(FrameDrawTarget target, ImguiFrameData input) noexcept;

    private:
        explicit UiInputHandler(ResourceRegistry& resources) noexcept : resources_(resources) {}

        ResourceRegistry& resources_;
    };
} // namespace lux::render::detail
