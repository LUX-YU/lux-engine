#pragma once

#include <lux/engine/description/UiDraw.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/inputs/DrawInputs.hpp>

namespace lux::engine::render
{
    struct FrameInput;
}

namespace lux::editor
{
    struct UiImageSelection final
    {
        rdesc::UiImageToken token;
        std::uint32_t index;
    };

    // Converts ImGui-independent coordinates/indices to one declared UI pass. Images are a cold,
    // token-sorted mapping into UiSceneInputs::images. Geometry is already frozen in frame
    // memory (or is otherwise retained through its Response); this function does not copy it again.
    [[nodiscard]] lux::render::RenderResult<lux::render::DrawListData>
    makeUiDrawInput(engine::render::FrameInput&, const rdesc::UiDrawData&, std::span<const UiImageSelection>) noexcept;
} // namespace lux::editor
