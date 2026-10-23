#pragma once

#include <lux/engine/description/UiDraw.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/graph/Draw.hpp>

namespace lux::rendering
{
    struct FrameInput;

    struct UiImageSelection final
    {
        rdesc::UiImageToken token;
        std::uint32_t index;
    };

    // Converts ImGui-independent coordinates/indices to one declared UI pass. Images are a cold,
    // token-sorted mapping into UiRasterFeatureConfig::images. Geometry is already frozen in frame
    // memory (or is otherwise retained through its Response); this function does not copy it again.
    [[nodiscard]] render::RenderResult<render::DrawListData>
    makeUiDrawInput(FrameInput&, const rdesc::UiDrawData&, std::span<const UiImageSelection>) noexcept;
} // namespace lux::rendering
