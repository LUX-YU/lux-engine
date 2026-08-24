#pragma once
// ============================================================================
//  MeshGpuCacheComponent.hpp — 「这个实体要画的网格与材质已经在 GPU 上了」。
//  运行期产物，与 `TextureGpuCacheComponent` / `AnimatorCacheComponent` 同款。
//
//    MeshComponent{mesh_asset_id, material_asset_id}   作者写的意图（存盘）
//          │  ResidencySubsystem 驻留胶水:边沿请求 + 等待名单送达
//          ▼
//    MeshGpuCacheComponent{mesh, material}             运行期产物
//          │
//          ▼
//    MeshInstanceSubsystem：拿两个句柄建实例 —— 不认识 asset_id、不认识 AssetManager
//
//  故意**不是反射组件**：装的是运行期 GPU 句柄，绝不该被序列化或出现在属性面板。
//
//  ── 为什么网格与材质在同一个组件里 ────────────────────────────────────────
//
//  因为消费者要的是「两者都就绪」这一个事实：`addMeshInstance` 一次带上两个句柄，
//  少一个就不能建。拆成两个组件的话，消费者得自己做「两个 try_get 都非空」的
//  合取，而「材质是 nil（作者就没设）」与「材质还没上传好」这两种状态还要各自
//  区分 —— 那正是此前 `ensureMaterial` 返回空句柄时那段
//  `if (!c.material_asset_id.is_nil() && mat.isNull())` 判断的由来。
//  放在一起，解析器把这个判断做掉一次，组件在 = 两者都可用。
// ============================================================================

#include <lux/engine/resource/asset/Asset.hpp>                        // asset_id_t
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>   // RMeshHandle / RMaterialHandle

namespace lux::ecs
{
    struct MeshGpuCacheComponent
    {
        lux::render::RMeshHandle     mesh{};
        /// 空句柄 = 作者没指定材质（合法：渲染侧有默认）。**不是**「还没好」——
        /// 还没好的时候整个组件都不存在。
        lux::render::RMaterialHandle material{};

        /// 产生它们的资产 id。解析器用来判「作者是不是换了资产」；消费者用来判
        /// 「已经建好的实例是不是该拆掉重建」（换资产 = remove + 下一帧重建）。
        lux::asset::asset_id_t       mesh_source{};
        lux::asset::asset_id_t       material_source{};
    };

} // namespace lux::ecs
