#pragma once
// ============================================================================
//  TriOverlayOperation.hpp — TriOverlay 通信外观的【作者声明】(A+)
//  瞬态三角覆盖层(gizmo 填充面):每帧整体替换,Blob 载荷骑 ResourceOp 环、
//  带上传回执 —— LineListOperation 的孪生。Operation 面由 engine_add_comm_ops
//  生成;手写残余 = handleTriOverlayUpload(TriOverlayOperationHandlers.cpp)。
//  生成 Proxy 的 uploadTriangles 收 (payload, blob_bytes, align) 原始字节面,
//  调用方自炊 as_bytes + alignof(GizmoVertex) 并保证 vertex_count 一致。
// ============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/client/features/gizmo/GizmoVertex.hpp>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    struct LUX_TYPE_INFO(both) LUX_COMM_CONFIG(
        prefix = TriOverlay,
        id = lux.render.tri_overlay.v1,
        display = TriOverlayTransient,
        feature = TriOverlayTransientFeature,
        feature_header = lux / engine / render / renderer / features / gizmo /
                         TriOverlayTransientFeature.hpp) TriOverlayTransientCommConfig
    {
        ShaderHandle vertex_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle fragment_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        uint32_t max_vertices{200'000};
    };
    static_assert(std::is_trivially_copyable_v<TriOverlayTransientCommConfig>);

    /// Reply for uploadTriangles — chunk id + status (0 = success).
    struct TriOverlayUploadedReply
    {
        uint32_t chunk_id{0};
        uint32_t status{0};
    };
    static_assert(std::is_trivially_copyable_v<TriOverlayUploadedReply>);

    struct LUX_OP(
        lane = program,
        kind = blob,
        name = TriOverlayUpload,
        method = uploadTriangles,
        reply = TriOverlayUploadedReply,
        opcode = resource) UploadTriOverlayPayload
    {
        RenderSceneId scene_id{};
        uint32_t chunk_id{0};
        uint32_t vertex_count{0}; ///< 必须等于 blob 字节数 / sizeof(GizmoVertex)
        LUX_OP_BLOB() BlobRef vertex_data {};
    };
    static_assert(std::is_trivially_copyable_v<UploadTriOverlayPayload>);

} // namespace lux::render
