#pragma once

#include <lux/engine/render/frame/FrameStorage.hpp>
#include <lux/engine/render/inputs/ImguiFrameData.hpp>
#include <lux/engine/systems/render/visibility.h>

namespace lux::engine::render
{
    // One copy of the branch's native arrays into reusable application frame blocks.
    // Call after ImGui::Render and before the next NewFrame; no live Context is retained.
    [[nodiscard]] LUX_ENGINE_SYSTEM_RENDER_PUBLIC lux::render::RenderResult<void>
    captureImguiFrame(
        lux::render::FrameWriter&,
        lux::render::FrameDataBinding<lux::render::ImguiFrameData>,
        const ImDrawData&,
        std::span<const lux::render::ImguiImage>
    ) noexcept;
} // namespace lux::editor
