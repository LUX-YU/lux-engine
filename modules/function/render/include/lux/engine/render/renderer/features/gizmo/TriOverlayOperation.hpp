#pragma once
#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/renderer/features/FeatureOps.hpp>
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/renderer/features/gizmo/GizmoVertex.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>
#include <span>
#include <string_view>

namespace lux::render
{
    struct FeatureFactory;
    class RenderSession;
    template <typename T> class RenderRequest;

    // =========================================================================
    //  CommConfig (trivially copyable, transferred as attachment)
    // =========================================================================
    struct TriOverlayTransientCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
        uint32_t     max_vertices{200'000};
    };
    static_assert(std::is_trivially_copyable_v<TriOverlayTransientCommConfig>);

    // =========================================================================
    //  Operation payloads
    // =========================================================================
    struct UploadTriOverlayPayload
    {
        RenderSceneId   scene_id{};
        uint32_t        chunk_id{0};
        uint32_t        vertex_count{0};
        BlobRef         vertex_data{};
    };
    static_assert(std::is_trivially_copyable_v<UploadTriOverlayPayload>);

    struct TriOverlayUploadedReply
    {
        uint32_t chunk_id{0};
        uint32_t status{0}; ///< 0 = success
    };
    static_assert(std::is_trivially_copyable_v<TriOverlayUploadedReply>);

    // =========================================================================
    //  CommandTraits — reply binding for replyToCurrent<>
    // =========================================================================
    template <>
    struct CommandTraits<UploadTriOverlayPayload>
    {
        using Reply = TriOverlayUploadedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<TriOverlayUploadedReply>;  // Q3: hash-derived
    };

    // =========================================================================
    //  Typed op declaration + ids. Blob op: vertex_data is the BlobRef field;
    //  carries a reply and rides the ResourceOp lifecycle ring like other uploads.
    // =========================================================================
    struct UploadTriOverlayOp
    {
        using Payload = UploadTriOverlayPayload;
        static constexpr EOpKind kind = EOpKind::Blob;
        static constexpr OpCode  opcode = opcodes::ResourceOp;
        static constexpr const char* name = "TriOverlayUpload";
        static constexpr auto blob_field = &UploadTriOverlayPayload::vertex_data;
    };

    using TriOverlayOperationIds = FeatureOpIds<UploadTriOverlayOp>;

    // =========================================================================
    //  FeatureFactory extern
    // =========================================================================
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kTriOverlayTransientFactory;

    // =========================================================================
    //  Client-side proxy
    // =========================================================================
    class LUX_FUNCTION_PUBLIC TriOverlayProxy
    {
    public:
        TriOverlayProxy(RenderSession& session, TriOverlayOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        RenderRequest<TriOverlayUploadedReply> uploadTriangles(
            RenderSceneId scene_id, uint32_t chunk_id,
            std::span<const GizmoVertex> vertices
        );

    private:
        RenderSession*         session_;
        TriOverlayOperationIds ops_;
    };

} // namespace lux::render
