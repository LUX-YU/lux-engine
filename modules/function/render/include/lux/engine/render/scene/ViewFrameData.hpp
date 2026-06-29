#pragma once

#include <lux/engine/render/core/RenderTypes.hpp>
#include <lux/engine/render/renderer/FrustumCuller.hpp>
#include <lux/engine/render/renderer/RenderViewTypes.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>

namespace lux::render
{

/**
 * @brief Per-view, per-frame data from game thread to render thread.
 *
 * Contains only per-view camera and viewport state.
 * Scene-global timing is carried through scene sync commands.
 */
struct LUX_FUNCTION_PUBLIC ViewFrameData
{
    // Per-view camera state
    CameraView      camera_view{};
    EntityTransform camera_transform{};
    Frustum         frustum{};

    // Per-view viewport state
    Viewport        viewport{};
    ScissorRect     scissor{};
    common::Size2D        extent{};

    void clear()
    {
        camera_view = {};
        camera_transform = {};
        frustum = {};
        viewport = {};
        scissor = {};
        extent = {};
    }
};

} // namespace lux::render

