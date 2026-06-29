#pragma once
/**
 * @file TrajectoryOperation.hpp
 * @brief Comm protocol payloads, FeatureFactory externs, operation IDs,
 *        query name constants, and client-side sugar for trajectory rendering.
 */

#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/renderer/features/FeatureOps.hpp>
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/cxx/container/SlotMap.hpp>

#include <cstdint>
#include <type_traits>
#include <span>
#include <algorithm>
#include <string_view>

namespace lux::render
{
    struct FeatureFactory;
    struct GenericOkReply;
    class RenderSession;
    template <typename T> class RenderRequest;

    struct TrajectoryHandleTag {};
    using TrajectoryHandle = lux::cxx::SlotKey<TrajectoryHandleTag>;
    static_assert(std::is_trivially_copyable_v<TrajectoryHandle>);
    // =========================================================================
    //  Default shader name constants for TrajectoryFeature
    // =========================================================================
    inline constexpr std::string_view kTrajectoryLineVertShaderName = "trajectory_line.vert";
    inline constexpr std::string_view kTrajectoryLineFragShaderName = "trajectory_line.frag";

    // =========================================================================
    //  Per-mode CommConfig structs
    // =========================================================================
    struct TrajectoryLineCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
        uint32_t max_global_vertices{1'000'000};
    };
    static_assert(std::is_trivially_copyable_v<TrajectoryLineCommConfig>);

    // =========================================================================
    //  Operation payloads
    // =========================================================================

    /// Create a new trajectory and upload initial points.
    struct CreateTrajectoryPayload
    {
        RenderSceneId scene_id{};
        uint32_t point_count{0};
        BlobRef point_data{}; ///< copied GpuTrajectoryVertex[] (stored in request payload)
    };
    static_assert(std::is_trivially_copyable_v<CreateTrajectoryPayload>);

    /// Append vertices to an existing trajectory.
    struct AppendTrajectoryPointsPayload
    {
        RenderSceneId scene_id{};
        TrajectoryHandle trajectory{};
        uint32_t point_count{0};
        BlobRef point_data{}; ///< copied GpuTrajectoryVertex[] (stored in request payload)
    };
    static_assert(std::is_trivially_copyable_v<AppendTrajectoryPointsPayload>);

    /// Clear a trajectory (reset vertex count to 0, keep slot alive).
    struct ClearTrajectoryPayload
    {
        RenderSceneId scene_id{};
        TrajectoryHandle trajectory{};
    };
    static_assert(std::is_trivially_copyable_v<ClearTrajectoryPayload>);

    /// Remove a trajectory (free its slot entirely).
    struct RemoveTrajectoryPayload
    {
        RenderSceneId scene_id{};
        TrajectoryHandle trajectory{};
    };
    static_assert(std::is_trivially_copyable_v<RemoveTrajectoryPayload>);

    /// Atomically replace all vertices in a trajectory (clear + upload in same frame).
    struct ReplaceTrajectoryPointsPayload
    {
        RenderSceneId scene_id{};
        TrajectoryHandle trajectory{};
        uint32_t point_count{0};
        BlobRef  point_data{}; ///< copied GpuTrajectoryVertex[] (stored in request payload)
    };
    static_assert(std::is_trivially_copyable_v<ReplaceTrajectoryPointsPayload>);

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
    //  CommandTraits — reply bindings for replyToCurrent<> (reply ids hash-derived).
    // =========================================================================
    template <>
    struct CommandTraits<CreateTrajectoryPayload>
    {
        using Reply = TrajectoryCreatedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<TrajectoryCreatedReply>;
    };

    template <>
    struct CommandTraits<AppendTrajectoryPointsPayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<GenericOkReply>;
    };

    template <>
    struct CommandTraits<ClearTrajectoryPayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<GenericOkReply>;
    };

    template <>
    struct CommandTraits<RemoveTrajectoryPayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<GenericOkReply>;
    };

    template <>
    struct CommandTraits<ReplaceTrajectoryPointsPayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<GenericOkReply>;
    };

    // =========================================================================
    //  Typed op declarations + ids. Create/Append/Replace carry a point blob on the
    //  ResourceOp ring; Clear/Remove are reply-bearing lifecycle ops kept on CommandOp
    //  (opcode override preserves the original rings). Order == registration order.
    // =========================================================================
    struct CreateTrajectoryOp
    {
        using Payload = CreateTrajectoryPayload;
        static constexpr EOpKind kind = EOpKind::Blob;
        static constexpr OpCode  opcode = opcodes::ResourceOp;
        static constexpr const char* name = "TrajectoryCreate";
        static constexpr auto blob_field = &CreateTrajectoryPayload::point_data;
    };
    struct AppendTrajectoryPointsOp
    {
        using Payload = AppendTrajectoryPointsPayload;
        static constexpr EOpKind kind = EOpKind::Blob;
        static constexpr OpCode  opcode = opcodes::ResourceOp;
        static constexpr const char* name = "TrajectoryAppend";
        static constexpr auto blob_field = &AppendTrajectoryPointsPayload::point_data;
    };
    struct ClearTrajectoryOp
    {
        using Payload = ClearTrajectoryPayload;
        static constexpr EOpKind kind = EOpKind::Resource;     // reply-bearing unary…
        static constexpr OpCode  opcode = opcodes::CommandOp;  // …kept on CommandOp
        static constexpr const char* name = "TrajectoryClear";
    };
    struct RemoveTrajectoryOp
    {
        using Payload = RemoveTrajectoryPayload;
        static constexpr EOpKind kind = EOpKind::Resource;
        static constexpr OpCode  opcode = opcodes::CommandOp;
        static constexpr const char* name = "TrajectoryRemove";
    };
    struct ReplaceTrajectoryPointsOp
    {
        using Payload = ReplaceTrajectoryPointsPayload;
        static constexpr EOpKind kind = EOpKind::Blob;
        static constexpr OpCode  opcode = opcodes::ResourceOp;
        static constexpr const char* name = "TrajectoryReplace";
        static constexpr auto blob_field = &ReplaceTrajectoryPointsPayload::point_data;
    };

    using TrajectoryOperationIds = FeatureOpIds<
        CreateTrajectoryOp, AppendTrajectoryPointsOp, ClearTrajectoryOp,
        RemoveTrajectoryOp, ReplaceTrajectoryPointsOp>;

    // =========================================================================
    //  FeatureFactory extern
    // =========================================================================

    extern LUX_FUNCTION_PUBLIC const FeatureFactory kTrajectoryLineFactory;

    // Strongly-typed point type exposed only via the proxy.
    // Must match GPU vertex layout (24 bytes): x,y,z + packed_color + time + width
    struct TrajectoryPoint
    {
        float x, y, z;
        uint32_t packed_color;
        float time;
        float width;

        static TrajectoryPoint make(
            float px, float py, float pz,
            float r, float g, float b, float a,
            float t, float w) noexcept
        {
            auto to_u8 = [](float v) -> uint32_t
            {
                return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            uint32_t packed = (to_u8(r) << 0) | (to_u8(g) << 8) | (to_u8(b) << 16) | (to_u8(a) << 24);
            return {px, py, pz, packed, t, w};
        }

        static uint32_t makeColor(float r, float g, float b, float a) noexcept
        {
            auto to_u8 = [](float v) -> uint32_t
            {
                return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            return (to_u8(r) << 0) | (to_u8(g) << 8) | (to_u8(b) << 16) | (to_u8(a) << 24);
        }
    };
    static_assert(sizeof(TrajectoryPoint) == 24, "TrajectoryPoint must be 24 bytes to match GPU layout");


    // =========================================================================
    //  Client-side proxy — TrajectoryProxy
    // =========================================================================
    class LUX_FUNCTION_PUBLIC TrajectoryProxy
    {
    public:
        TrajectoryProxy(RenderSession& session, TrajectoryOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Create a new trajectory and upload initial points.
        RenderRequest<TrajectoryCreatedReply> newTrajectory(
            RenderSceneId scene_id,
            std::span<const TrajectoryPoint> points
        );

        /// Append vertices to an existing trajectory.
        RenderRequest<GenericOkReply> appendPoints(
            RenderSceneId scene_id, TrajectoryHandle trajectory,
            std::span<const TrajectoryPoint> points
        );

        /// Clear a trajectory (reset to 0 vertices, keep slot alive).
        RenderRequest<GenericOkReply> clear(RenderSceneId scene_id, TrajectoryHandle trajectory);

        /// Remove a trajectory from the GPU.
        RenderRequest<GenericOkReply> remove(RenderSceneId scene_id, TrajectoryHandle trajectory);

        /// Atomically replace all vertices in a trajectory (same-frame clear + upload, no flicker).
        RenderRequest<GenericOkReply> replacePoints(
            RenderSceneId scene_id, TrajectoryHandle trajectory,
            std::span<const TrajectoryPoint> points);

    private:
        RenderSession*         session_;
        TrajectoryOperationIds ops_;
    };

} // namespace lux::render
