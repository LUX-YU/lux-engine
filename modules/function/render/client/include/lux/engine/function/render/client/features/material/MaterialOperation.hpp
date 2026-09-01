#pragma once
// ============================================================================
//  MaterialOperation.hpp — StandardMaterialFeature factory + feature-scoped
//  material ops (uploadGraphMaterial / modifyGraphMaterial / destroyMaterial).
//
//  The global MaterialResources stack is a
//  FEATURE domain (Stage D-2). The commands that create / mutate / destroy
//  materials are dispatched by dynamically-allocated TypeIds (register_ops_fn —
//  the grid / light pattern) and sent via a feature-scoped MaterialProxy. The
//  core RenderProtocol no longer names material ops. A 2D / unlit / headless
//  scene that omits StandardMaterial carries none of this vocabulary.
//
//  uploadGraphMaterial REPLIES with the new RMaterialHandle. Its ExternalDataRef
//  points into packet-owned storage created by pushOwnedBytesCopy(), so the
//  bytes remain alive through server consumption. Generic borrowed attachments
//  instead must outlive server consumption; submit returning is insufficient,
//  and a reply is the safe completion proxy. modify COPIES its BlobRef payload.
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
// TypeId, opcodes, CommandTraits, ExternalDataRef, BlobRef
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>             // RMaterialHandle
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>                   // ShaderHandle
#include <lux/engine/function/render/client/resources/ops/ResourceOperationCommon.hpp> // DestroyResourcePayload
#include <lux/engine/function/render/client/protocol/FeatureOps.hpp>
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <span>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;
    class RenderProgramSession;
    class RenderUploadSession;
    enum class ERenderUploadSubmitError : std::uint8_t;
    template <typename T> class RenderRequest;

    // =========================================================================
    //  Material command payloads + reply (moved out of the core RenderProtocol.hpp —
    //  the core no longer names them; this feature header is their SSOT now).
    // =========================================================================
    /// Destroy 载荷:平铺自有结构(原为 DestroyResourcePayload 别名 —— 注解挂不上
    /// using;线上布局逐字节同旧)。
    struct LUX_OP(lane = control, kind = stream, name = DestroyMaterial, method = destroyMaterial, opcode = resource)
        DestroyMaterialPayload
    {
        RMaterialHandle handle{};
    };
    static_assert(std::is_trivially_copyable_v<DestroyMaterialPayload>);

    /// Node-graph (Graph family) material upload. The descriptor is a flat
    /// GraphMaterialData blob addressed by ExternalDataRef and backed by the
    /// upload packet's owned attachment storage.
    struct LUX_OP(
        lane = upload,
        kind = resource,
        name = UploadGraphMaterial,
        method = uploadGraphMaterial,
        reply = MaterialUploadedReply,
        manual_client = true) UploadGraphMaterialPayload
    {
        ExternalDataRef graph_desc{};        // points to const GraphMaterialData
        ShaderHandle graph_gbuffer_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){}; // R1: per-material baked frags → own bucket/PSO
        ShaderHandle graph_forward_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        uint64_t shader_key{0};
        uint32_t alpha_mode{0}; // rdesc::EAlphaMode: 0=Opaque,1=Mask,2=Blend
        uint8_t double_sided{0};
    };
    static_assert(std::is_trivially_copyable_v<UploadGraphMaterialPayload>);

    /// In-place per-frame update of an existing Graph material (fire-and-forget).
    /// graph_desc is an in-payload BLOB (copied at push time), NOT a borrow.
    struct LUX_OP(
        lane = upload,
        kind = blob,
        name = ModifyGraphMaterial,
        method = modifyGraphMaterial,
        opcode = resource) ModifyGraphMaterialPayload
    {
        RMaterialHandle handle{};
        LUX_OP_BLOB() BlobRef graph_desc {}; // GraphMaterialData copied into the payload blob
    };
    static_assert(std::is_trivially_copyable_v<ModifyGraphMaterialPayload>);

    /// Reply for uploadGraphMaterial — the new material handle + status.
    struct MaterialUploadedReply
    {
        RMaterialHandle handle{};
        uint32_t status{0};
    };
    static_assert(std::is_trivially_copyable_v<MaterialUploadedReply>);

    /// 无客户端创建参数 —— 空 tag 承载特性身份。
    struct LUX_TYPE_INFO(both) LUX_COMM_CONFIG(
        prefix = Material,
        id = lux.render.material.v1,
        display = StandardMaterial,
        feature = StandardMaterialFeature,
        feature_header = lux / engine / render / renderer / features / material /
                         StandardMaterialFeature.hpp) MaterialCommTag
    {
    };
    static_assert(std::is_trivially_copyable_v<MaterialCommTag>);

    class MaterialProxy; // 生成于 comm/genops/MaterialOperation.ops.hpp
    class MaterialControlClient;
    class MaterialUploadClient;

    // ── 便捷面(§7.5,同名自由函数,定义在 Handlers.cpp)──────────────────
    //    upload:pushOwnedBytesCopy() supplies packet-owned ExternalDataRef storage.
    //    A generic borrowed attachment must outlive server consumption; a reply
    //    is the safe completion proxy, not submitFrame() returning.
    //    连带 shader_key 稳定哈希;modify:GraphMaterialData 逐帧拷贝进 blob。
    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::cxx::expected<RenderRequest<MaterialUploadedReply>, ERenderUploadSubmitError>
    uploadGraphMaterial(MaterialUploadClient client, const GraphMaterialData& data);
    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::cxx::expected<RenderRequest<MaterialUploadedReply>, ERenderUploadSubmitError>
    uploadGraphMaterial(
        MaterialUploadClient client,
        const GraphMaterialData& data,
        ShaderHandle gbuffer_shader,
        ShaderHandle forward_shader,
        std::uint32_t alpha_mode = 0,
        bool double_sided = false
    );
    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::cxx::expected<void, ERenderUploadSubmitError>
    modifyGraphMaterial(MaterialUploadClient client, RMaterialHandle handle, const GraphMaterialData& data);

} // namespace lux::render
