#pragma once
/**
 * @file GizmoVertex.hpp
 * @brief Public CPU-side vertex for gizmo line-list / triangle-overlay uploads.
 *
 * External users build `GizmoVertex`es and hand them to
 * `LineListProxy::uploadLines` / `TriOverlayProxy::uploadTriangles`. This is
 * the public counterpart to the point-cloud feature's `PointCloudPoint`:
 * a 16-byte POD whose layout matches the gizmo vertex shader input
 *   layout(location = 0) in vec3  in_position;    // offset 0,  12 bytes
 *   layout(location = 1) in uint  in_packed_attr;  // offset 12, 4 bytes
 *
 * The GPU-side representation lives in the render module's private
 * `sinclude/` and is never exposed across the public API boundary.
 */

#include <algorithm>
#include <cstdint>

namespace lux::render
{
    /// Public 16-byte gizmo vertex (position + packed RGBA color).
    struct GizmoVertex
    {
        float x, y, z;        ///< World-space position (12 bytes)
        uint32_t packed_attr; ///< RGBA packed: R[7:0] G[15:8] B[23:16] A/I[31:24]

        /// Pack normalized [0,1] RGBA channels into a single uint32_t.
        static constexpr uint32_t pack(float r, float g, float b, float a = 1.0f) noexcept
        {
            auto to_u8 = [](float v) constexpr -> uint32_t {
                return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            return (to_u8(r) << 0) | (to_u8(g) << 8) | (to_u8(b) << 16) | (to_u8(a) << 24);
        }

        static constexpr GizmoVertex
        make(float px, float py, float pz, float r, float g, float b, float a = 1.0f) noexcept
        {
            return {px, py, pz, pack(r, g, b, a)};
        }
    };

    static_assert(sizeof(GizmoVertex) == 16, "GizmoVertex must be 16 bytes to match GPU layout");

} // namespace lux::render
