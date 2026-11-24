#pragma once

#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/runtime/commands/Command.hpp>

namespace lux::render
{
    struct UpdateFrameData final
    {
        static constexpr std::string_view name = "render.frame.update_data";
        struct Request final { FrameData data; };
        struct Response final
        {
            RenderError error{};
            FrameData data;
        };
    };
} // namespace lux::render
