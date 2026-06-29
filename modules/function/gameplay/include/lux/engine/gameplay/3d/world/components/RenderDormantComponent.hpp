#pragma once
/**
 * @file RenderDormantComponent.hpp
 * @brief Runtime tag marking a renderable entity that world streaming has
 *        put to sleep (its cell is out of the streaming range).
 *
 * 大世界① 增量2 W2a —— cell 级区块流送(GPU 桥门控)。
 *
 * 语义:带此标签 = 该可渲染实体当前被 WorldStreamingSystem 判为"休眠"
 * (所在 cell 离流送源太远)。entt→GPU 桥的 mesh 适配器在 drive 阶段用
 * `entt::exclude<RenderDormantComponent>` 把它挡在 view 外,并在 reap 阶段
 * 移除它仍持有的 GPU 实例 —— 于是休眠实体不占 GPU 实例 / 网格驻留,GPU
 * 内存随活跃 cell 数缩放(= W2 的可扩展性目标)。
 *
 * **缺失即照常渲染**。这是 opt-in、非破坏式:没有 WorldStreamingSystem 在跑
 * 时,没有实体会被打标签,每个可渲染实体都和以前一样桥接到 GPU。
 *
 * 与 WorldTransformComponent 一样属于运行期派生态 —— **不做反射注册、
 * 永不写进 .luxscene**(它不是用户编辑的数据,而是每帧由距离判定生成)。
 */

namespace lux::gameplay::d3
{
    /// Empty tag — see file header. Present ⇒ excluded from the render bridge.
    struct RenderDormantComponent
    {
    };

} // namespace lux::gameplay::d3
