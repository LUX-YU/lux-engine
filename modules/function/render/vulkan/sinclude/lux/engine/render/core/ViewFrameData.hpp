#pragma once

#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>
#include <lux/engine/render/core/FrustumCuller.hpp>
#include <lux/engine/function/render/client/core/RenderViewTypes.hpp>
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
        CameraView camera_view{};
        EntityTransform camera_transform{};
        RenderLargePosition3D render_origin{};
        float coordinate_page_size{1024.0f};
        Frustum frustum{};

        // Per-view viewport state
        Viewport viewport{};
        ScissorRect scissor{};
        lux::math::Extent2u extent{};

        void clear()
        {
            camera_view = {};
            camera_transform = {};
            render_origin = {};
            coordinate_page_size = 1024.0f;
            frustum = {};
            viewport = {};
            scissor = {};
            extent = {};
        }
    };

} // namespace lux::render
