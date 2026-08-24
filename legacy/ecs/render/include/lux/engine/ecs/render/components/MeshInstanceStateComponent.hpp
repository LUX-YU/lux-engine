#pragma once
// ============================================================================
//  MeshInstanceStateComponent.hpp — 一个实体的 GPU 网格实例的**逐实体状态**
//  (lux::ecs)。与 MeshGpuCacheComponent / MeshInstanceReadyComponent 同类:
//  纯运行期状态,**故意不进反射清单**(绝不序列化、不进属性面板)。
//
//  ── 为什么它是组件,而不是子系统里的一张 unordered_map ────────────────────
//
//  它此前是 `MeshInstanceSubsystem` 私有的
//  `std::unordered_map<entity_id, Live>`。那是一份**影子存储**:
//
//    · 每帧每实体一次哈希 + 指针跳转。2083 个实体的 demo 场景里,
//      这是 74.8 µs 中可观的一块(见 `.internal/reactive-extraction-design.zh-CN.md` §1);
//    · 不能被 view 迭代 —— 于是「对所有实例重发可见性」「骨骼调色板累加」
//      这类**真需要全量**的场合只能走多池交集,而不是一次连续遍历;
//    · 不随实体销毁 —— 靠观察者手工对账;
//    · 不能被检视/调试/快照。
//
//  逐实体数据的归属就是组件池。把它放回去,上面每一条都自然消失。
//
//  ── ⚠️ 它同时是**离场信号的锚点** ───────────────────────────────────────
//
//  「这个实体有没有 GPU 实例」由**本组件是否存在**回答,不必反查任何表。
//  于是归还路径变成一条 `on_destroy<MeshInstanceStateComponent>`:
//
//    · 实体被销毁      → EnTT 逐组件发 on_destroy,句柄在信号里**仍然可读**
//                        (`ecs/core/test/reactive_storage_probe.cpp` ⑧ 是实证锚点);
//    · 实体离开集合    → 子系统在安全点 `remove<>` 本组件,同一条信号照常触发。
//
//  两条路径一个处理器,不存在「某条离场路径漏挂观察者」这种只表现为资源泄漏、
//  不报错的错法。
//
//  ⚠️ **不要**把它挂给「实体被销毁时 on_destroy<MeshComponent> 里去读它」——
//  `registry.destroy` 清各个池的**顺序不保证**,那样读到的可能是已经清掉的池。
//  锚点必须是本组件自己的 on_destroy。
//
//  按策略模板化:`MeshInstanceSubsystem` 有静态网格与骨骼网格两个实例化,
//  它们的状态必须是**不同的组件类型**,否则同一个实体上会互相覆盖。
// ============================================================================

#include <cstdint>

#include <lux/engine/resource/asset/Asset.hpp>                              // asset_id_t
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>         // RMeshHandle / RMaterialHandle
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>  // RenderObjectHandle

namespace lux::ecs
{
    template <class Traits>
    struct MeshInstanceStateComponent
    {
        /// 服务端的实例句柄 —— 归还路径要还的就是它。
        lux::render::RenderObjectHandle object{};
        /// 建实例时用的网格句柄。骨骼批处理要用;静态网格只用于换代判据。
        lux::render::RMeshHandle        mesh{};
        /// 建实例时用的材质句柄(热更新批5):内容变更走「id 不变、缓存重建、
        /// **句柄变**」这条路,只比 id 的换资产判据看不见它 —— 实例会拿着
        /// 已销毁的旧句柄继续画。与 mesh 句柄一起进 swapped 判据。
        lux::render::RMaterialHandle    material{};
        /// 这个实例最后一次 `makeInstanceVisibleForView` 是对**哪一代** view 发的。
        /// 曾经是个 `bool` —— 那在「全场景一个 view、终生不变」的前提下没问题,
        /// 而相机拥有 view 之后前提没了:进 Play 会销毁编辑器相机的 view、建游戏
        /// 相机的,bool 闩会让所有已存在的实例**对新 view 一次都不发**,场景渲染
        /// 空白且不报任何错。0 = 从未发过(真实代次从 1 起)。
        std::uint32_t                   visible_for_view_gen{0};
        /// 建实例时用的资产来源 id。换代判据的另一半;实体死后组件已经没了,
        /// 所以这两个 id 必须存在**实例这边**,不能到时候回去读作者组件。
        lux::asset::asset_id_t          mesh_id{};
        lux::asset::asset_id_t          material_id{};
        /// 上一次推给服务端的实例标志位。差分比对的基准,单写者。
        std::uint32_t                   last_flags{0};
        /// Explicit visual transition parameters captured when this instance
        /// was created.  Identity and streaming origin do not imply a fade.
        std::uint32_t                   transition_milliseconds{0u};
        std::uint32_t                   transition_seed{0u};
        /// Exact spatial value last submitted to the render owner.  This is
        /// the extraction-side diff baseline; without it a flags-only change
        /// also emits an unconditional transform update and the verification
        /// oracle correctly reports untracked work on the following frame.
        lux::render::RenderSpatialTransform3D last_transform{};
    };

} // namespace lux::ecs
