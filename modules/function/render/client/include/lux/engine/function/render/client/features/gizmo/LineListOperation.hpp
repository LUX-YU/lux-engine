#pragma once
// ============================================================================
//  LineListOperation.hpp — LineList 通信外观的【作者声明】(A+)
//  瞬态线列表(gizmo/调试画线):每帧整体替换,Blob 载荷骑 ResourceOp 环、
//  带上传回执。Operation 面由 engine_add_comm_ops 生成到
//  <lux/engine/function/render/client/genops/LineListOperation.ops.hpp|.ops.cpp>;
//  手写残余 = handleLineListUpload(LineListOperationHandlers.cpp,含 blob
//  解码与 replyToCurrent)。
//
//  生成 Proxy 的 uploadLines 收 (payload, blob_bytes, align) 原始字节面 ——
//  调用方自炊 std::as_bytes(vertices) + alignof(GizmoVertex),并负责
//  vertex_count 与 span 长度一致(服务端 assert 看住)。
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
    /// CommConfig (trivially copyable, transferred as attachment).
    struct LUX_COMM_CONFIG(
        prefix = LineList,
        id = lux.render.line_list.v1,
        display = LineListTransient,
        feature = LineListTransientFeature,
        feature_header = lux / engine / render / renderer / features / gizmo /
                         LineListTransientFeature.hpp) LineListTransientCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
        float line_width{1.5f};
        uint32_t max_vertices{200'000};
    };
    static_assert(std::is_trivially_copyable_v<LineListTransientCommConfig>);

    /// Reply for uploadLines — chunk id + status (0 = success).
    struct LineListUploadedReply
    {
        uint32_t chunk_id{0};
        uint32_t status{0};
    };
    static_assert(std::is_trivially_copyable_v<LineListUploadedReply>);

    struct LUX_OP(
        lane = frame,
        kind = blob,
        name = LineListUpload,
        method = uploadLines,
        reply = LineListUploadedReply,
        opcode = resource) UploadLineListPayload
    {
        RenderSceneId scene_id{};
        uint32_t chunk_id{0};
        uint32_t vertex_count{0}; ///< 必须等于 blob 字节数 / sizeof(GizmoVertex)
        LUX_OP_BLOB() BlobRef vertex_data {};
    };
    static_assert(std::is_trivially_copyable_v<UploadLineListPayload>);

} // namespace lux::render
