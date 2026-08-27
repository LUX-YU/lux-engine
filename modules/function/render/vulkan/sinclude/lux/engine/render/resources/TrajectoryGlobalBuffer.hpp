#pragma once
/**
 * @file TrajectoryGlobalBuffer.hpp
 * @brief 全部轨迹路径共用的统一顶点缓冲。
 *
 * 竞技场机制(缓冲创建、freelist 分配与合并、槽位表、按槽上传、整缓冲扩容)
 * 归 SlotArenaBuffer<GpuTrajectoryVertex>;本类只加轨迹专属的两件事:
 *   - ensureSlotCapacity:扩容**保留**旧数据(append 语义),含 #26 的 RAW 修复;
 *   - append:往槽位尾部追加。
 * 其余是领域命名的转发访问器(vertex / trajectory 用词)。
 *
 * 内存布局:
 *   [slot_0: first=0,  capacity=C0] GpuTrajectoryVertex[C0]
 *   [slot_1: first=C0, capacity=C1] GpuTrajectoryVertex[C1]
 *   ...
 *
 * 线程:全部方法仅渲染线程。
 */

#include <lux/engine/render/resources/SlotArenaBuffer.hpp>
#include <lux/engine/render/resources/TrajectoryGpuData.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>

namespace lux::render
{
    class TransferScheduler;

    class LUX_FUNCTION_PUBLIC TrajectoryGlobalBuffer final : public SlotArenaBuffer<GpuTrajectoryVertex>
    {
        using Base = SlotArenaBuffer<GpuTrajectoryVertex>;

    public:
        static constexpr uint32_t kInvalidTrajectoryId = Base::kInvalidId;

        /// @param max_vertices 缓冲的总顶点容量。
        bool init(VmaAllocator allocator, uint32_t max_vertices)
        {
            return Base::init(allocator, max_vertices);
        }

        /// 确保 @p trajectory_id 至少有 @p capacity 个顶点的槽位。
        ///
        /// 与点云侧的同名方法**语义不同**:这里扩容会把已有顶点搬到新区间并保留
        /// vertex_count(轨迹是不断 append 的),点云那边扩容即丢弃。差异正是它没能
        /// 进基类的原因。
        bool ensureSlotCapacity(uint32_t trajectory_id, uint32_t capacity, TransferScheduler& scheduler);

        /// 往槽位尾部追加,返回实际写入数(容量不足时截断,满则返回 0)。
        uint32_t
        append(uint32_t trajectory_id, std::span<const GpuTrajectoryVertex> data, TransferScheduler& scheduler);

        // ── 领域命名转发 ────────────────────────────────────────────────────
        void clearTrajectory(uint32_t trajectory_id) noexcept
        {
            resetSlot(trajectory_id);
        }
        void setVertexCount(uint32_t trajectory_id, uint32_t count) noexcept
        {
            setCount(trajectory_id, count);
        }

        [[nodiscard]] uint32_t maxVertices() const noexcept
        {
            return maxElements();
        }
        [[nodiscard]] uint32_t usedVertices() const noexcept
        {
            return usedElements();
        }

        /// 遍历全部活跃轨迹。回调:void fn(uint32_t trajectory_id, const Slot&)
        template <typename Fn> void forEachTrajectory(Fn&& fn) const
        {
            forEachSlot(std::forward<Fn>(fn));
        }
    };

} // namespace lux::render
