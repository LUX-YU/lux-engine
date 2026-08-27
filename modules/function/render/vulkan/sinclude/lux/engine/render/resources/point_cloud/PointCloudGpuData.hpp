#pragma once

#include <cstdint>
#include <cstring>
#include <cassert>
#include <algorithm>

namespace lux::render
{
    // =====================================================================
    // GpuPointVertex — 16 bytes, matches the pointcloud_simple / pointcloud_lod vert input layout
    // =====================================================================

    /// Compact GPU vertex for point cloud rendering (16 bytes).
    ///
    /// Must exactly match the vertex shader input:
    ///   layout(location = 0) in vec3  in_position;     // offset 0,  12 bytes
    ///   layout(location = 1) in uint  in_packed_attr;   // offset 12, 4 bytes
    ///
    /// Packed attribute layout (matches shader unpackColor):
    ///   bits [7:0]   = R  (0–255)
    ///   bits [15:8]  = G  (0–255)
    ///   bits [23:16] = B  (0–255)
    ///   bits [31:24] = Intensity (0–255)
    struct GpuPointVertex
    {
        float x, y, z;        ///< World-space position (12 bytes)
        uint32_t packed_attr; ///< RGBA packed: R[7:0] G[15:8] B[23:16] I[31:24]

        /// Pack normalized [0,1] RGBA channels into a single uint32_t.
        static constexpr uint32_t pack(float r, float g, float b, float intensity = 1.0f) noexcept
        {
            auto to_u8 = [](float v) constexpr -> uint32_t {
                return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            return (to_u8(r) << 0) | (to_u8(g) << 8) | (to_u8(b) << 16) | (to_u8(intensity) << 24);
        }

        /// Construct a complete vertex from position + color + intensity.
        static constexpr GpuPointVertex
        make(float px, float py, float pz, float r, float g, float b, float intensity = 1.0f) noexcept
        {
            return {px, py, pz, pack(r, g, b, intensity)};
        }
    };
    static_assert(
        sizeof(GpuPointVertex) == 16,
        "GpuPointVertex must be 16 bytes to match the pointcloud_simple / pointcloud_lod vert layout");

    // =====================================================================
    // GPU Octree Node — 96 bytes, matches pointcloud_culling.comp
    // =====================================================================

    /// GPU-side octree node structure.
    /// Must match GPUOctreeNode in pointcloud_culling.comp (set 0, binding 0).
    /// Layout: std430-compatible, 96 bytes per node.
    struct alignas(16) GpuOctreeNode
    {
        float bbox_min[3];
        float pad0;
        float bbox_max[3];
        float pad1;

        uint32_t first_point;   ///< Index into point SSBO
        uint32_t point_count;   ///< Number of points in this node
        uint32_t children_mask; ///< Bitmask of which children exist (8 bits)
        uint32_t stream_state;  ///< 0=unloaded, 1=loading, 2=ready

        float lod_error;         ///< Geometric error for LOD selection
        uint32_t depth_level;    ///< Depth in the octree
        uint32_t morton_code_lo; ///< Low 32 bits of 64-bit Morton code
        uint32_t morton_code_hi; ///< High 32 bits of 64-bit Morton code

        float avg_color[3];  ///< Average RGB color of points in this node
        float avg_intensity; ///< Average intensity

        uint32_t padding[2]; ///< Padding to 96 bytes
    };
    static_assert(sizeof(GpuOctreeNode) == 96, "GpuOctreeNode must be 96 bytes to match shader layout");

    // =====================================================================
    // Point Cloud Instance — 16 bytes, culling output
    // =====================================================================

    /// GPU-side point cloud instance (output of culling compute shader).
    /// Must match PointCloudInstance in pointcloud_culling.comp (set 0, binding 1).
    struct alignas(16) PointCloudInstance
    {
        uint32_t first_point; ///< Starting index in point SSBO
        uint32_t point_count; ///< Number of points to render
        uint32_t node_id;     ///< Source octree node index
        float lod_error;      ///< LOD error for adaptive rendering
    };
    static_assert(sizeof(PointCloudInstance) == 16, "PointCloudInstance must be 16 bytes to match shader layout");

    // =====================================================================
    // UBO Structs — must match std140 layouts in shaders
    // =====================================================================

    /// Frame constants for point cloud rendering (pointcloud_simple / pointcloud_lod vert, set 0, binding 0).
    struct alignas(16) PointCloudFrameConstants
    {
        float view_proj_matrix[16];    ///< Column-major 4×4 view-projection matrix
        float camera_position[3];      ///< World-space camera position
        float point_size;              ///< Base point size in pixels
        float camera_direction[3];     ///< World-space camera forward direction
        float lod_bias;                ///< LOD bias multiplier (default 1.0)
        uint32_t max_points_per_frame; ///< Budget cap on total rendered points
        uint32_t frame_index;          ///< Current frame number
        float time;                    ///< Elapsed time in seconds
        float delta_time;              ///< Frame delta time in seconds
    };

    /// Culling parameters (pointcloud_culling.comp, set 0, binding 3).
    struct alignas(16) CullingParams
    {
        float view_proj_matrix[16];
        float camera_position[3];
        float lod_bias;
        float camera_direction[3];
        float pixel_error_threshold;
        uint32_t max_points_per_frame;
        uint32_t max_instances;
        float time;
        float delta_time;
    };

    /// CPU-side camera/view state shared between frame constants and culling params.
    /// Not a GPU struct — used to fill both UBOs consistently.
    struct PointCloudCameraState
    {
        const float* view_proj = nullptr; ///< 16 floats, column-major 4×4
        float camera_position[3]{};
        float camera_direction[3]{};
        float lod_bias = 1.0f;
        float time = 0.0f;
        float delta_time = 0.0f;
        uint32_t max_points_per_frame = 10'000'000;
    };

    /// Apply common camera state to PointCloudFrameConstants.
    inline void applyViewState(PointCloudFrameConstants& dst, const PointCloudCameraState& src) noexcept
    {
        if (src.view_proj)
            std::memcpy(dst.view_proj_matrix, src.view_proj, 64);
        std::memcpy(dst.camera_position, src.camera_position, 12);
        std::memcpy(dst.camera_direction, src.camera_direction, 12);
        dst.lod_bias = src.lod_bias;
        dst.time = src.time;
        dst.delta_time = src.delta_time;
        dst.max_points_per_frame = src.max_points_per_frame;
    }

    /// Apply common camera state to CullingParams.
    inline void applyViewState(CullingParams& dst, const PointCloudCameraState& src) noexcept
    {
        if (src.view_proj)
            std::memcpy(dst.view_proj_matrix, src.view_proj, 64);
        std::memcpy(dst.camera_position, src.camera_position, 12);
        std::memcpy(dst.camera_direction, src.camera_direction, 12);
        dst.lod_bias = src.lod_bias;
        dst.time = src.time;
        dst.delta_time = src.delta_time;
        dst.max_points_per_frame = src.max_points_per_frame;
    }

    // =====================================================================
    // Frustum, Instance Header, Indirect Draw Command
    // =====================================================================

    /// GPU-side frustum planes for culling.
    /// Matches FrustumPlanes in pointcloud_culling.comp (set 0, binding 4).
    struct alignas(16) FrustumPlanes
    {
        float planes[6][4]; ///< 6 planes: (nx, ny, nz, d) where nx·x + ny·y + nz·z + d = 0
    };

    /// Instance buffer header (matches SSBO layout with atomic counter).
    struct InstanceBufferHeader
    {
        uint32_t instance_count;
        uint32_t padding[3];
    };

    /// Indirect draw command (matches VkDrawIndirectCommand layout).
    struct IndirectDrawCommand
    {
        uint32_t vertex_count;
        uint32_t instance_count;
        uint32_t first_vertex;
        uint32_t first_instance;
    };
    static_assert(sizeof(IndirectDrawCommand) == 16, "IndirectDrawCommand must match VkDrawIndirectCommand layout");

    /// Header for the indirect draw buffer (used with vkCmdDrawIndirectCount).
    /// Layout: [IndirectDrawHeader] [IndirectDrawCommand commands[max_instances]]
    /// The culling shader atomically increments draw_count and appends one command per visible node.
    struct IndirectDrawHeader
    {
        uint32_t draw_count; ///< Number of valid draw commands (atomic counter)
        uint32_t padding[3]; ///< Align commands to 16 bytes
    };

    static_assert(sizeof(IndirectDrawHeader) == 16, "IndirectDrawHeader must be 16 bytes for alignment");

} // namespace lux::render
