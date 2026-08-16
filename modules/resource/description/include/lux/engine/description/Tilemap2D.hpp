#pragma once
/**
 * @file Tilemap2D.hpp
 * @brief 2D 瓦片地图的**共享编码约定** —— 组件层与渲染层都要认的那一份。
 *
 * 为什么在 description 而不是任何一边:`kEmptyTile` 是 R16_UNORM 索引纹理的
 * texel 编码,组件负责**存**这张表、渲染特性负责**传**它,两边说的必须是同一件事。
 * 它此前住在 `render/.../canvas2d/Canvas2DOperation.hpp`,于是
 * `TilemapComponent`(一个 POD 组件头)为了一个 `std::uint16_t` 把一个 9-include
 * 的 render feature 头整个拉了进来 —— 方向也反了:组件是数据、桥与特性是行为,
 * 依赖只能是"行为看见数据"。
 *
 * description 是两边共同的下游,所以常量定义在这里,两边各自 include。
 * 注意这里**没有**在 render / ecs 侧留任何 re-export 别名:那种转发正是
 * F-1 批要删的东西。
 */

#include <cstdint>

namespace lux::rdesc
{
    /// 瓦片 id 中表示"这里没有瓦片"的值。
    ///
    /// 编码为 0xFFFF/65535 = 1.0 —— unorm 的最大值,而真实瓦片序号恒
    /// < cols*rows ≤ 0xFFFF,所以它取不到,可以安全当哨兵。
    inline constexpr std::uint16_t kEmptyTile = 0xFFFFu;

} // namespace lux::rdesc
