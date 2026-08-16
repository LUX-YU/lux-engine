#pragma once
/**
 * @file PointCloudGlobalBuffer.hpp
 * @brief 全部点云 chunk 共用的统一 SSBO / 顶点缓冲(取代按叶子各自建 VkBuffer)。
 *
 * 所有点云渲染模式(GPU-Driven / LOD / Splatting / MeshShader)共享这条缓冲;
 * 切换模式**不需要**重传任何点数据。
 *
 * 竞技场机制(缓冲创建、freelist 分配与合并、槽位表、按槽上传、整缓冲扩容)
 * 归 SlotArenaBuffer<GpuPointVertex>;本类只加点云专属的 ensureSlotCapacity
 * (扩容**丢弃**旧数据的 replace 语义),其余是领域命名(point / chunk 用词)的
 * 转发访问器。
 *
 * 内存布局:
 *   [slot_0: first=0,  capacity=C0] GpuPointVertex[C0]
 *   [slot_1: first=C0, capacity=C1] GpuPointVertex[C1]
 *   ...
 *
 * 分配策略:变长槽位上的简单 freelist。整理(compaction)有意不做在
 * insert/delete 路径上,保持它们 O(1)。
 *
 * 线程:全部方法仅渲染线程。
 */

#include <lux/engine/render/resources/SlotArenaBuffer.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>

namespace lux::render
{
    class TransferScheduler;

/**
 * @brief 多模式点云特性共用的统一 GPU 点缓冲。
 *
 * @code
 *   PointCloudGlobalBuffer buf;
 *   buf.init(allocator, 4'000'000);   // 预留 4M 个点位
 *
 *   // 叶子变脏时(渲染线程):
 *   buf.ensureSlotCapacity(cid, leaf.alive_count, scheduler);
 *   buf.upload(cid, packed_verts, scheduler);
 * @endcode
 */
class LUX_FUNCTION_PUBLIC PointCloudGlobalBuffer final
    : public SlotArenaBuffer<GpuPointVertex>
{
    using Base = SlotArenaBuffer<GpuPointVertex>;

public:
    static constexpr uint32_t kInvalidChunkId = Base::kInvalidId;

    /// @param max_points 缓冲的总点容量。
    bool init(VmaAllocator allocator, uint32_t max_points) { return Base::init(allocator, max_points); }

    /// 确保 @p chunk_id 至少有 @p capacity 个点的槽位。
    ///
    /// 与轨迹侧的同名方法**语义不同**:这里扩容会把 point_count 归零、**不搬**
    /// 旧数据(点云是整块 replace 的,扩容后紧跟一次全量 upload),轨迹那边要保留。
    /// 差异正是它没能进基类的原因。
    ///
    /// 容量不足时可能触发整缓冲扩容;扩容拷贝以 priority=-1 提交,由调度器的
    /// 中段屏障负责 TRANSFER→TRANSFER 同步。
    bool ensureSlotCapacity(uint32_t chunk_id, uint32_t capacity, TransferScheduler& scheduler);

    // ── 领域命名转发 ────────────────────────────────────────────────────
    /// 只更新存活点数,不产生 GPU 拷贝(alive 标志变了但无需重传的元数据路径)。
    void setPointCount(uint32_t chunk_id, uint32_t point_count) noexcept { setCount(chunk_id, point_count); }

    [[nodiscard]] uint32_t maxPoints()  const noexcept { return maxElements(); }
    [[nodiscard]] uint32_t usedPoints() const noexcept { return usedElements(); }
};

} // namespace lux::render
