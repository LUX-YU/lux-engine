#pragma once
#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/renderer/features/FeatureOps.hpp>
#include <lux/engine/render/core/FeatureHandle.hpp>
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>

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

    // =========================================================================
    //  Default shader name constants for PointCloudFeature
    // =========================================================================
    inline constexpr std::string_view kPCSimpleVertShaderName  = "pointcloud_simple.vert";
    inline constexpr std::string_view kPCSimpleFragShaderName  = "pointcloud_simple.frag";
    inline constexpr std::string_view kPCCullingCompShaderName = "pointcloud_culling.comp";
    inline constexpr std::string_view kPCLodVertShaderName     = "pointcloud_lod.vert";
    inline constexpr std::string_view kPCSplatFragShaderName   = "pointcloud_splat.frag";

    // =========================================================================
    //  Per-mode CommConfig structs (trivially copyable, transferred as attachments)
    // =========================================================================
    struct PCSimpleCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
        float    initial_point_size{3.0f};
        uint32_t max_global_points{4'000'000};
        uint32_t max_octree_nodes{65'536};
    };
    static_assert(std::is_trivially_copyable_v<PCSimpleCommConfig>);

    struct PCGPUDrivenCommConfig
    {
        ShaderHandle compute_shader{};
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
        float    initial_point_size{3.0f};
        uint32_t max_nodes{65'536};
    };
    static_assert(std::is_trivially_copyable_v<PCGPUDrivenCommConfig>);

    struct PCLODCommConfig
    {
        ShaderHandle compute_shader{};
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
        float    point_size_world{0.05f};
        float    min_size{1.0f};
        float    max_size{20.0f};
        uint32_t max_nodes{65'536};
    };
    static_assert(std::is_trivially_copyable_v<PCLODCommConfig>);

    struct PCSplattingCommConfig
    {
        ShaderHandle compute_shader{};
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
        float    point_size_world{0.05f};
        float    min_size{2.0f};
        float    max_size{30.0f};
        uint32_t max_nodes{65'536};
    };
    static_assert(std::is_trivially_copyable_v<PCSplattingCommConfig>);

    struct PCTransientCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
        float    point_size{3.0f};
        uint32_t max_points{2'000'000};
    };
    static_assert(std::is_trivially_copyable_v<PCTransientCommConfig>);

    // =========================================================================
    //  Operation payloads (resource ops)
    // =========================================================================

    // Strongly-typed point exposed by the proxy. Must match GPU vertex layout (16 bytes).
    struct PointCloudPoint
    {
        float    x, y, z;
        uint32_t packed_attr;

        static constexpr uint32_t pack(float r, float g, float b, float intensity = 1.0f) noexcept
        {
            auto to_u8 = [](float v) constexpr -> uint32_t {
                return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            return (to_u8(r) <<  0) |
                   (to_u8(g) <<  8) |
                   (to_u8(b) << 16) |
                   (to_u8(intensity) << 24);
        }

        static constexpr PointCloudPoint make(
            float px, float py, float pz,
            float r, float g, float b,
            float intensity = 1.0f) noexcept
        {
            return { px, py, pz, pack(r,g,b,intensity) };
        }
    };
    static_assert(sizeof(PointCloudPoint) == 16, "PointCloudPoint must be 16 bytes to match GPU layout");

    struct UploadPointCloudChunkPayload
    {
        RenderSceneId   scene_id{};
        uint32_t        chunk_id{0};
        uint32_t        point_count{0};
        BlobRef         point_data{};     ///< copied PointCloudPoint[] stored in request payload
    };
    static_assert(std::is_trivially_copyable_v<UploadPointCloudChunkPayload>);

    struct RemovePointCloudChunkPayload
    {
        RenderSceneId   scene_id{};
        uint32_t chunk_id{0};
    };
    static_assert(std::is_trivially_copyable_v<RemovePointCloudChunkPayload>);

    /// Clear all point cloud chunks for a scene (free all slots + octree nodes).
    struct ClearAllPointCloudPayload
    {
        RenderSceneId scene_id{};
    };
    static_assert(std::is_trivially_copyable_v<ClearAllPointCloudPayload>);

    /// Clear a single chunk (reset point_count to 0, keep slot allocated).
    struct ClearPointCloudChunkPayload
    {
        RenderSceneId scene_id{};
        uint32_t chunk_id{0};
    };
    static_assert(std::is_trivially_copyable_v<ClearPointCloudChunkPayload>);

    /// Runtime point-size adjustment.
    ///
    /// Semantics per render mode:
    ///   - Simple / GPUDriven / Transient : screen-pixel size (assigned directly).
    ///   - LOD / Splatting                : screen-pixel upper clamp (lod_pc.max_size);
    ///                                      world radius and min clamp keep their
    ///                                      original Config values so depth scaling
    ///                                      is preserved.
    struct SetPointCloudPointSizePayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        float         point_size{3.0f};
    };
    static_assert(std::is_trivially_copyable_v<SetPointCloudPointSizePayload>);

    // =========================================================================
    //  Reply
    // =========================================================================

    struct PointCloudChunkUploadedReply
    {
        uint32_t chunk_id{0};
        uint32_t status{0}; ///< 0 = success
    };
    static_assert(std::is_trivially_copyable_v<PointCloudChunkUploadedReply>);

    // =========================================================================
    //  CommandTraits — reply binding for replyToCurrent<> (reply ids hash-derived).
    // =========================================================================
    template <>
    struct CommandTraits<UploadPointCloudChunkPayload>
    {
        using Reply = PointCloudChunkUploadedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<PointCloudChunkUploadedReply>;
    };

    template <>
    struct CommandTraits<ClearAllPointCloudPayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<GenericOkReply>;
    };

    template <>
    struct CommandTraits<ClearPointCloudChunkPayload>
    {
        using Reply = GenericOkReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<GenericOkReply>;
    };

    // Remove / SetPointSize are fire-and-forget (no reply) — the default CommandTraits
    // (has_reply=false) applies, no specialization needed.

    // =========================================================================
    //  Typed op declarations + ids. Upload carries a point blob on ResourceOp; the
    //  clears are reply-bearing lifecycle ops kept on CommandOp; remove / setPointSize
    //  are fire-and-forget. Order == registration order (== the FeatureOpIds slots).
    // =========================================================================
    struct UploadPointCloudChunkOp
    {
        using Payload = UploadPointCloudChunkPayload;
        static constexpr EOpKind kind = EOpKind::Blob;
        static constexpr OpCode  opcode = opcodes::ResourceOp;
        static constexpr const char* name = "PointCloudUpload";
        static constexpr auto blob_field = &UploadPointCloudChunkPayload::point_data;
    };
    struct RemovePointCloudChunkOp
    {
        using Payload = RemovePointCloudChunkPayload;
        static constexpr EOpKind kind = EOpKind::Stream;   // CommandOp, fire-and-forget
        static constexpr const char* name = "PointCloudRemove";
    };
    struct ClearAllPointCloudOp
    {
        using Payload = ClearAllPointCloudPayload;
        static constexpr EOpKind kind = EOpKind::Resource;     // reply-bearing unary…
        static constexpr OpCode  opcode = opcodes::CommandOp;  // …kept on CommandOp
        static constexpr const char* name = "PointCloudClearAll";
    };
    struct ClearPointCloudChunkOp
    {
        using Payload = ClearPointCloudChunkPayload;
        static constexpr EOpKind kind = EOpKind::Resource;
        static constexpr OpCode  opcode = opcodes::CommandOp;
        static constexpr const char* name = "PointCloudClearChunk";
    };
    struct SetPointCloudPointSizeOp
    {
        using Payload = SetPointCloudPointSizePayload;
        static constexpr EOpKind kind = EOpKind::Stream;   // CommandOp, fire-and-forget
        static constexpr const char* name = "PointCloudSetPointSize";
    };

    using PointCloudOperationIds = FeatureOpIds<
        UploadPointCloudChunkOp, RemovePointCloudChunkOp, ClearAllPointCloudOp,
        ClearPointCloudChunkOp, SetPointCloudPointSizeOp>;

    // =========================================================================
    //  Per-mode FeatureFactory externs
    // =========================================================================

    extern LUX_FUNCTION_PUBLIC const FeatureFactory kPCFeatureSimpleFactory;
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kPCFeatureGPUDrivenFactory;
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kPCFeatureLODFactory;
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kPCFeatureSplattingFactory;
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kPCFeatureTransientFactory;

    // =========================================================================
    //  Client-side proxy — PointCloudProxy
    // =========================================================================
    class LUX_FUNCTION_PUBLIC PointCloudProxy
    {
    public:
        PointCloudProxy(RenderSession& session, PointCloudOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Upload (or replace) a point cloud chunk. Data is copied into the request payload.
        /// If the chunk already exists, its contents are overwritten (ensureSlotCapacity + upload).
        RenderRequest<PointCloudChunkUploadedReply> uploadChunk(
            RenderSceneId scene_id, 
            uint32_t chunk_id,
            std::span<const PointCloudPoint> points
        );

        /// Remove a point cloud chunk from the GPU.
        void removeChunk(RenderSceneId scene_id, uint32_t chunk_id);

        /// Clear all point cloud chunks for a scene (free all slots + octree nodes).
        /// Any same-frame uploadChunk calls queued after this will be processed normally.
        RenderRequest<GenericOkReply> clearAll(RenderSceneId scene_id);

        /// Reset a single chunk (point_count=0, keep slot allocated for reuse).
        RenderRequest<GenericOkReply> clearChunk(RenderSceneId scene_id, uint32_t chunk_id);

        /// Adjust the screen-pixel point size for the target feature.
        /// See SetPointCloudPointSizePayload for per-mode semantics.
        void setPointSize(RenderSceneId scene_id, FeatureHandle feature, float point_size);

    private:
        RenderSession*          session_;
        PointCloudOperationIds  ops_;
    };

} // namespace lux::render
