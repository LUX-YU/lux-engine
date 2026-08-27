#pragma once
// ============================================================================
//  TrajectoryOperation.hpp — Trajectory 通信外观的【作者声明】(A+)
//
//  5 个 op 三种形状:Create/Append/Replace = Blob 载荷骑 ResourceOp 环带回执;
//  Clear/Remove = 回执型生命周期 op 留在 CommandOp 环(opcode 覆写保持原环)。
//  声明序 = 注册序 = wire 序。Operation 面由 engine_add_comm_ops 生成到
//  <lux/engine/function/render/client/genops/TrajectoryOperation.ops.hpp|.ops.cpp>;
//  手写残余 = handleTrajectory*(TrajectoryOperationHandlers.cpp)。
//
//  生成 Proxy 的 Blob 方法收 (payload, blob_bytes, align) 原始字节面 ——
//  调用方自炊 std::as_bytes(points) + alignof(TrajectoryPoint),并保证
//  point_count 与 span 长度一致(服务端 assert 看住)。
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lux::render
{
    // TrajectoryHandle/TrajectoryHandleTag 现定义在 core/ResourceHandle.hpp
    // (本头已包含它)—— 标识类型属基础层,存储层不必为一个句柄包含整个操作头。
    static_assert(std::is_trivially_copyable_v<TrajectoryHandle>);

    // =========================================================================
    //  Default shader name constants for TrajectoryFeature
    // =========================================================================
    inline constexpr std::string_view kTrajectoryLineVertShaderName = "trajectory_line.vert";
    inline constexpr std::string_view kTrajectoryLineFragShaderName = "trajectory_line.frag";

    struct LUX_COMM_CONFIG(
        prefix = Trajectory,
        id = lux.render.trajectory_line.v1,
        display = TrajectoryLine,
        feature = TrajectoryLineFeature,
        feature_header = lux / engine / render / renderer / features / trajectory /
                         TrajectoryLineFeature.hpp) TrajectoryLineCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
        uint32_t max_global_vertices{1'000'000};
    };
    static_assert(std::is_trivially_copyable_v<TrajectoryLineCommConfig>);

    // =========================================================================
    //  Reply
    // =========================================================================
    struct TrajectoryCreatedReply
    {
        TrajectoryHandle trajectory{};
        uint32_t status{0}; ///< 0 = success
    };
    static_assert(std::is_trivially_copyable_v<TrajectoryCreatedReply>);

    // =========================================================================
    //  Op payloads(声明序 = 注册序)
    // =========================================================================

    /// Create a new trajectory and upload initial points.
    struct LUX_OP(
        lane = upload,
        kind = blob,
        name = TrajectoryCreate,
        method = newTrajectory,
        reply = TrajectoryCreatedReply,
        opcode = resource) CreateTrajectoryPayload
    {
        RenderSceneId scene_id{};
        uint32_t point_count{0}; ///< 必须等于 blob 字节数 / sizeof(TrajectoryPoint)
        LUX_OP_BLOB() BlobRef point_data {};
    };
    static_assert(std::is_trivially_copyable_v<CreateTrajectoryPayload>);

    /// Append vertices to an existing trajectory.
    struct LUX_OP(
        lane = upload,
        kind = blob,
        name = TrajectoryAppend,
        method = appendPoints,
        reply = GenericOkReply,
        opcode = resource) AppendTrajectoryPointsPayload
    {
        RenderSceneId scene_id{};
        TrajectoryHandle trajectory{};
        uint32_t point_count{0};
        LUX_OP_BLOB() BlobRef point_data {};
    };
    static_assert(std::is_trivially_copyable_v<AppendTrajectoryPointsPayload>);

    /// Clear a trajectory (reset vertex count to 0, keep slot alive).
    struct LUX_OP(
        lane = control,
        kind = resource,
        name = TrajectoryClear,
        method = clear,
        reply = GenericOkReply,
        opcode = command) ClearTrajectoryPayload
    {
        RenderSceneId scene_id{};
        TrajectoryHandle trajectory{};
    };
    static_assert(std::is_trivially_copyable_v<ClearTrajectoryPayload>);

    /// Remove a trajectory (free its slot entirely).
    struct LUX_OP(
        lane = control,
        kind = resource,
        name = TrajectoryRemove,
        method = remove,
        reply = GenericOkReply,
        opcode = command) RemoveTrajectoryPayload
    {
        RenderSceneId scene_id{};
        TrajectoryHandle trajectory{};
    };
    static_assert(std::is_trivially_copyable_v<RemoveTrajectoryPayload>);

    /// Atomically replace all vertices in a trajectory (clear + upload in same frame).
    struct LUX_OP(
        lane = upload,
        kind = blob,
        name = TrajectoryReplace,
        method = replacePoints,
        reply = GenericOkReply,
        opcode = resource) ReplaceTrajectoryPointsPayload
    {
        RenderSceneId scene_id{};
        TrajectoryHandle trajectory{};
        uint32_t point_count{0};
        LUX_OP_BLOB() BlobRef point_data {};
    };
    static_assert(std::is_trivially_copyable_v<ReplaceTrajectoryPointsPayload>);

    // =========================================================================
    //  Strongly-typed point(客户端便捷类型,语义糖留作者头)
    //  Must match GPU vertex layout (24 bytes): x,y,z + packed_color + time + width
    // =========================================================================
    struct TrajectoryPoint
    {
        float x, y, z;
        uint32_t packed_color;
        float time;
        float width;

        static TrajectoryPoint
        make(float px, float py, float pz, float r, float g, float b, float a, float t, float w) noexcept
        {
            auto to_u8 = [](float v) -> uint32_t {
                return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            uint32_t packed = (to_u8(r) << 0) | (to_u8(g) << 8) | (to_u8(b) << 16) | (to_u8(a) << 24);
            return {px, py, pz, packed, t, w};
        }

        static uint32_t makeColor(float r, float g, float b, float a) noexcept
        {
            auto to_u8 = [](float v) -> uint32_t {
                return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            return (to_u8(r) << 0) | (to_u8(g) << 8) | (to_u8(b) << 16) | (to_u8(a) << 24);
        }
    };
    static_assert(sizeof(TrajectoryPoint) == 24, "TrajectoryPoint must be 24 bytes to match GPU layout");

} // namespace lux::render
