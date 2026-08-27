#include <lux/engine/render/resources/TrajectoryGlobalBuffer.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>

namespace lux::render
{

    bool
    TrajectoryGlobalBuffer::ensureSlotCapacity(uint32_t trajectory_id, uint32_t capacity, TransferScheduler& scheduler)
    {
        assert(isInitialized());
        if (capacity == 0)
            return false;

        if (!slots_.contains(trajectory_id))
        {
            // 首次分配给一个 64 顶点下限:轨迹是逐帧 append 的,起步就按请求量精确
            // 分配会让前几十次 append 各触发一次搬迁。
            const uint32_t init_cap = std::max(capacity, 64u);
            const uint32_t first = allocOrGrow(init_cap, scheduler);
            if (first == kInvalidId)
                return false;
            slots_.insert(trajectory_id, Slot{first, init_cap, 0});
            return true;
        }

        Slot& slot = slots_.at(trajectory_id);
        if (slot.capacity >= capacity)
            return true;

        const uint32_t grow_cap = std::max(capacity, slot.capacity * 2u);
        // Capture the buffer BEFORE allocOrGrow: if it grows, growBuffer() replaces
        // buffer_ with a new VkBuffer and submits the whole-buffer old->new copy at
        // priority -1. Reading the slot-relocation copy below from the NEW buffer
        // would race that growth copy (both priority -1, no barrier between same-
        // priority copies -> RAW: the relocation could read bytes the growth copy
        // hasn't written yet). The old buffer is only deferred-retired, so it is
        // still alive this frame; relocate FROM it instead — no dependency on the
        // growth copy. When no grow happens, pre_grow_buffer == buffer_ (a normal
        // same-buffer, non-overlapping relocation). (#26)
        const VkBuffer pre_grow_buffer = buffer_;
        const uint32_t first = allocOrGrow(grow_cap, scheduler);
        if (first == kInvalidId)
            return false;

        // 搬迁已有顶点:轨迹扩容保留数据(与点云侧的丢弃语义相反)。
        const uint32_t old_first = slot.first;
        const uint32_t old_count = slot.count;
        if (old_count > 0)
        {
            scheduler.submitBufferCopy({
                .src = pre_grow_buffer,
                .src_offset = elemBytes(old_first),
                .dst = buffer_,
                .dst_offset = elemBytes(first),
                .size = elemBytes(old_count),
                .domain = EBufferDomain::TransferDst,
                .priority = -1,
            }
            );
        }

        deferReturn(old_first, slot.capacity);
        slot.first = first;
        slot.capacity = grow_cap;
        slot.count = old_count;
        return true;
    }

    uint32_t TrajectoryGlobalBuffer::append(
        uint32_t trajectory_id,
        std::span<const GpuTrajectoryVertex> data,
        TransferScheduler& scheduler
    )
    {
        if (data.empty())
            return 0;
        if (!slots_.contains(trajectory_id))
            return 0;

        Slot& slot = slots_.at(trajectory_id);
        const uint32_t remaining = slot.capacity - slot.count;
        if (remaining == 0)
            return 0;

        const uint32_t to_append = std::min(static_cast<uint32_t>(data.size()), remaining);
        const VkDeviceSize byte_offset = elemBytes(slot.first + slot.count);
        const VkDeviceSize byte_size = elemBytes(to_append);

        StagingAlloc stg = scheduler.allocateStaging(byte_size);
        if (!stg.mapped)
            return 0;

        std::memcpy(stg.mapped, data.data(), byte_size);
        scheduler.submitBufferCopy({
            .src = stg.buffer,
            .src_offset = stg.srcOffset,
            .dst = buffer_,
            .dst_offset = byte_offset,
            .size = byte_size,
            .domain = EBufferDomain::VertexInput_CS,
            .priority = 0,
        }
        );

        slot.count += to_append;
        return to_append;
    }

} // namespace lux::render
