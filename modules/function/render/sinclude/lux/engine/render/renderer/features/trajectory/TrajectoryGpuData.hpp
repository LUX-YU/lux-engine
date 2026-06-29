#pragma once
/**
 * @file TrajectoryGpuData.hpp
 * @brief GPU data structures for multi-mode trajectory rendering.
 *
 * Defines vertex formats for:
 *   - Input vertices (GpuTrajectoryVertex, 24 bytes) — stored in TrajectoryGlobalBuffer
 *   - Expanded vertices (GpuExpandedVertex, 32 bytes) — output of ribbon/tube compute shaders
 *   - Indirect draw structures for compute-driven modes
 */

#include <cstdint>
#include <algorithm>

namespace lux::render
{
    // =========================================================================
    //  GpuTrajectoryVertex — 24 bytes, input vertex for all trajectory modes
    // =========================================================================

    /// Compact GPU vertex for trajectory rendering (24 bytes).
    ///
    /// Shader input layout:
    ///   layout(location = 0) in vec3  inPosition;     // offset 0,  12 bytes
    ///   layout(location = 1) in uint  inPackedColor;   // offset 12, 4 bytes
    ///   layout(location = 2) in float inTime;          // offset 16, 4 bytes
    ///   layout(location = 3) in float inWidth;         // offset 20, 4 bytes
    struct GpuTrajectoryVertex
    {
        float x, y, z;         ///< World-space position (12 bytes)
        uint32_t packed_color; ///< RGBA packed: R[7:0] G[15:8] B[23:16] A[31:24]
        float time;            ///< Normalized arc-length parameter [0,1] for gradient/fade
        float width;           ///< Per-vertex width (line width / ribbon half-width / tube radius)

        /// Pack normalized [0,1] RGBA channels into a single uint32_t.
        static constexpr uint32_t packColor(float r, float g, float b, float a = 1.0f) noexcept
        {
            auto to_u8 = [](float v) constexpr -> uint32_t
            {
                return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            return (to_u8(r) << 0) |
                   (to_u8(g) << 8) |
                   (to_u8(b) << 16) |
                   (to_u8(a) << 24);
        }

        /// Construct a complete vertex.
        static constexpr GpuTrajectoryVertex make(
            float px, float py, float pz,
            float r, float g, float b, float a,
            float t, float w) noexcept
        {
            return {px, py, pz, packColor(r, g, b, a), t, w};
        }
    };
    static_assert(sizeof(GpuTrajectoryVertex) == 24, "GpuTrajectoryVertex must be 24 bytes to match shader layout");

    // =========================================================================
    //  GpuExpandedVertex — 32 bytes, output of ribbon/tube compute shaders
    // =========================================================================

    /// Expanded vertex produced by compute shaders for ribbon and tube modes.
    ///
    /// Shader input layout:
    ///   layout(location = 0) in vec3  inPosition;    // offset 0,  12 bytes
    ///   layout(location = 1) in uint  inPackedColor;  // offset 12, 4 bytes
    ///   layout(location = 2) in vec3  inNormal;       // offset 16, 12 bytes
    ///   layout(location = 3) in float inUV_V;         // offset 28, 4 bytes
    struct GpuExpandedVertex
    {
        float x, y, z;         ///< World-space position (12 bytes)
        uint32_t packed_color; ///< RGBA packed (same as GpuTrajectoryVertex)
        float nx, ny, nz;      ///< World-space normal (for lighting in tube mode)
        float uv_v;            ///< V coordinate across ribbon/tube width [0,1]
    };
    static_assert(sizeof(GpuExpandedVertex) == 32, "GpuExpandedVertex must be 32 bytes to match shader layout");

    // =========================================================================
    //  Indirect draw structures for compute-driven trajectory modes
    // =========================================================================

    /// Per-trajectory indirect draw command (matches VkDrawIndirectCommand).
    struct TrajectoryDrawCommand
    {
        uint32_t vertex_count;
        uint32_t instance_count;
        uint32_t first_vertex;
        uint32_t first_instance;
    };
    static_assert(sizeof(TrajectoryDrawCommand) == 16, "TrajectoryDrawCommand must be 16 bytes to match VkDrawIndirectCommand");

    /// Header for indirect draw buffer (used with vkCmdDrawIndirectCount).
    struct TrajectoryIndirectHeader
    {
        uint32_t draw_count; ///< Number of valid draw commands (atomic counter in compute)
        uint32_t padding[3]; ///< Align commands to 16 bytes
    };
    static_assert(sizeof(TrajectoryIndirectHeader) == 16, "TrajectoryIndirectHeader must be 16 bytes to match VkDrawIndirectCommand");

} // namespace lux::render
