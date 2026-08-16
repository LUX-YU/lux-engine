#pragma once
// ============================================================================
//  SkinningOperation.hpp — Skinning 通信外观的【作者声明】(A+)
//
//  GPU 蒙皮(骨骼调色板)是 FEATURE 域,不是核心协议 op:单实例上传 +
//  逐帧批量两条命令在此声明,动态 TypeId 派发(register_ops_fn 的 grid
//  范式),核心 RenderProtocol.hpp 不再认识 skinning(契约 C5)。
//
//  Operation 面由 engine_add_comm_ops 生成;手写残余 = handleUploadBone*
//  语义(SkinningOperationHandlers.cpp)。批量 op 是**双 blob**形状
//  (entries + bones 两段变长)—— 生成方法按字段声明序收
//  (payload, entries_bytes+align, bones_bytes+align) 并逐个回填 BlobRef。
//
//  requires=mesh_stack:蒙皮补丁写的是 MeshStack 装配的实例表;没有它,
//  本特性处于 Enabled 却每帧空转且零报错 —— 静默空转是最坏的失败,声明
//  required 后缺装即明确拒绝(理由详版见旧 handlers 注释,语义保留于此)。
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    /// 蒙皮无客户端可调创建参数(计算着色器自装内置)—— 空配置结构只承载
    /// 特性身份注解;生成 createFn 以默认 Config 装配,与旧手写等价。
    struct LUX_COMM_CONFIG(prefix=Skinning, id=lux.render.skinning.v1, display=Skinning,
                           feature=SkinningFeature,
                           feature_header=lux/engine/render/renderer/features/skinning/SkinningFeature.hpp,
                           requires=lux.render.mesh_stack.v1)
    SkinningCommConfig
    {
    };
    static_assert(std::is_trivially_copyable_v<SkinningCommConfig>);

    // ---- Per-instance bone palette upload (GPU skinning) ----
    // Fire-and-forget CommandOp carrying a variable-length bone-matrix blob.
    // The handler resolves the instance's input vertex range from `mesh`, uploads
    // the palette + records a skinning dispatch, and patches the instance's
    // InstanceProperty.vertex_pool_id/base to the skinned output.
    struct LUX_OP(lane=frame, kind=blob, name=UploadBonePalette, method=uploadBonePalette)
    UploadBonePalettePayload
    {
        RenderSceneId      scene_id{};
        RenderObjectHandle object{};       ///< instance to skin (patched in place)
        RMeshHandle        mesh{};         ///< source mesh → input vertex range
        uint32_t           bone_count{0};
        // In-payload BLOB (copied into the ring-slot blob at push time), NOT a
        // borrowed pointer: fire-and-forget per-frame command; the consumer
        // advances the SPSC ring before the handler reads, so a borrow would race
        // the game thread clearing/reallocating its bone buffer next tick.
        LUX_OP_BLOB() BlobRef bones{};     ///< bone_count × BoneMatrixGpu (mat4, 64B)
    };
    static_assert(std::is_trivially_copyable_v<UploadBonePalettePayload>);

    // ---- Batched per-instance bone palettes (one command per frame) ----
    // `entries` is entry_count × BoneBatchEntry; `bones` is the concatenated
    // BoneMatrixGpu blob, each entry slicing [bone_offset, bone_offset+bone_count).
    struct BoneBatchEntry
    {
        RenderObjectHandle object{};       ///< instance to skin (patched in place)
        RMeshHandle        mesh{};         ///< source mesh → input vertex range
        uint32_t           bone_offset{0}; ///< first bone in the shared blob (mat4 units)
        uint32_t           bone_count{0};
    };
    static_assert(std::is_trivially_copyable_v<BoneBatchEntry>);

    struct LUX_OP(lane=frame, kind=stream, name=UploadBoneBatch, method=uploadBoneBatch)
    UploadBoneBatchPayload
    {
        RenderSceneId   scene_id{};
        uint32_t        entry_count{0};
        LUX_OP_BLOB() BlobRef entries{};   ///< entry_count × BoneBatchEntry
        LUX_OP_BLOB() BlobRef bones{};     ///< concatenated BoneMatrixGpu (mat4, 64 B each)
    };
    static_assert(std::is_trivially_copyable_v<UploadBoneBatchPayload>);

} // namespace lux::render
