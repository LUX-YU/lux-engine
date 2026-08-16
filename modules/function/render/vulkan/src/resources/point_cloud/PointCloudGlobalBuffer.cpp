#include <lux/engine/render/resources/point_cloud/PointCloudGlobalBuffer.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>

#include <cassert>

namespace lux::render
{

bool PointCloudGlobalBuffer::ensureSlotCapacity(uint32_t chunk_id,
                                                uint32_t capacity,
                                                TransferScheduler& scheduler)
{
    assert(isInitialized());
    if (capacity == 0) return false;

    if (!slots_.contains(chunk_id))
    {
        const uint32_t first = allocOrGrow(capacity, scheduler);
        if (first == kInvalidId) return false;
        slots_.insert(chunk_id, Slot{first, capacity, 0});
        return true;
    }

    Slot& slot = slots_.at(chunk_id);
    if (slot.capacity >= capacity)
        return true;

    const uint32_t first = allocOrGrow(capacity, scheduler);
    if (first == kInvalidId) return false;

    // 不搬旧数据:点云的扩容是"整块 replace 前的准备",调用方紧接着会 upload
    // 全量新数据,所以 count 归零。(轨迹侧相反 —— 见 TrajectoryGlobalBuffer。)
    deferReturn(slot.first, slot.capacity);
    slot.first    = first;
    slot.capacity = capacity;
    slot.count    = 0;
    return true;
}

} // namespace lux::render
