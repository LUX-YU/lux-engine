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
    //  Operation name constants
    // =========================================================================
    // =========================================================================
    //  CommConfig (trivially copyable, transferred as attachment)
    // =========================================================================
    struct LineListTransientCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
        float        line_width{1.5f};
        uint32_t     max_vertices{200'000};
    };
    static_assert(std::is_trivially_copyable_v<LineListTransientCommConfig>);

    // =========================================================================
    //  Operation payloads
    // =========================================================================
    struct UploadLineListPayload
    {
        RenderSceneId   scene_id{};
        uint32_t        chunk_id{0};
        uint32_t        vertex_count{0};
        BlobRef         vertex_data{};
    };
    static_assert(std::is_trivially_copyable_v<UploadLineListPayload>);

    struct LineListUploadedReply
    {
        uint32_t chunk_id{0};
        uint32_t status{0}; ///< 0 = success
    };
    static_assert(std::is_trivially_copyable_v<LineListUploadedReply>);

    // =========================================================================
    //  CommandTraits — reply binding for replyToCurrent<>
    // =========================================================================
    template <>
    struct CommandTraits<UploadLineListPayload>
    {
        using Reply = LineListUploadedReply;
        static constexpr bool has_reply = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<LineListUploadedReply>;  // Q3: hash-derived
    };

    // =========================================================================
    //  Typed op declaration + ids. Blob op: vertex_data is the BlobRef field;
    //  carries a reply and rides the ResourceOp lifecycle ring like other uploads.
    // =========================================================================
    struct UploadLineListOp
    {
        using Payload = UploadLineListPayload;
        static constexpr EOpKind kind = EOpKind::Blob;
        static constexpr OpCode  opcode = opcodes::ResourceOp;
        static constexpr const char* name = "LineListUpload";
        static constexpr auto blob_field = &UploadLineListPayload::vertex_data;
    };

    using LineListOperationIds = FeatureOpIds<UploadLineListOp>;

    // =========================================================================
    //  FeatureFactory extern
    // =========================================================================
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kLineListTransientFactory;

    // =========================================================================
    //  Client-side proxy
    // =========================================================================
    class LUX_FUNCTION_PUBLIC LineListProxy
    {
    public:
        LineListProxy(RenderSession& session, LineListOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        RenderRequest<LineListUploadedReply> uploadLines(
            RenderSceneId scene_id, uint32_t chunk_id,
            std::span<const GizmoVertex> vertices);

    private:
        RenderSession*       session_;
        LineListOperationIds ops_;
    };

} // namespace lux::render
