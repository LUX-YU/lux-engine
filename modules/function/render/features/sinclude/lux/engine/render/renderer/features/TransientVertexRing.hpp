#pragma once
/**
 * @file TransientVertexRing.hpp
 * @brief The ONE host-mapped per-frame-in-flight vertex ring features share.
 *
 * Three features (point-cloud transient, gizmo line-list, Canvas2D) each hand-rolled
 * this exact object — the same FrameSlot triple, the same vmaCreateBuffer(HOST_ACCESS_
 * SEQUENTIAL_WRITE | MAPPED) loop, the same retire-on-detach fix applied three times by
 * hand (#17). This type is that object ONCE, keeping the two lifecycle rules that were
 * repeatedly re-learned encoded in the API:
 *
 *  1. SLOT SELECTION takes the engine's REAL frame_index (slotFor(ctx.frame_index)) —
 *     never a private counter, which desyncs from the actual in-flight set (feature
 *     disable/re-enable, skipped frames, multi-scene) and lets the CPU overwrite a slot
 *     the GPU is still reading.
 *  2. RUNTIME DETACH must retire buffers through the frames-in-flight deferred-destroy
 *     queue (frames N-1/N-2 still bind them) — retireInto() nulls the handles so the
 *     destructor's immediate destroy() is a no-op on that path; destroy() remains the
 *     no-detach fallback (device idle at shutdown).
 *
 * Creation is fallible (Expected + rollback of partial allocations). Per-slot FEATURE
 * data (draw counts, content serials) deliberately does not live here — keep it in a
 * parallel array indexed by slotIndexFor(): the ring owns GPU lifecycle, nothing else.
 */

#include <lux/engine/function/render/client/core/Errors.hpp> // Expected / renderFailure

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace lux::render
{
    class TransientVertexRing
    {
    public:
        struct Slot
        {
            VkBuffer buffer{VK_NULL_HANDLE};
            VmaAllocation alloc{nullptr};
            void* mapped{nullptr};
        };

        TransientVertexRing() = default;
        ~TransientVertexRing()
        {
            destroy();
        }
        TransientVertexRing(const TransientVertexRing&) = delete;
        TransientVertexRing& operator=(const TransientVertexRing&) = delete;

        /// Create one host-mapped VERTEX_BUFFER slot of @p slot_bytes per frame-in-flight
        /// (at least one). On any slot's failure the already-created slots are rolled
        /// back and the ring is left empty.
        [[nodiscard]] Expected<void>
        create(VmaAllocator allocator, std::uint32_t frames_in_flight, VkDeviceSize slot_bytes)
        {
            allocator_ = allocator;
            const std::uint32_t count = std::max<std::uint32_t>(1u, frames_in_flight);
            slots_.assign(count, Slot{});
            for (auto& slot : slots_)
            {
                VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = slot_bytes;
                bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_AUTO;
                aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

                VmaAllocationInfo info{};
                const VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &slot.buffer, &slot.alloc, &info);
                if (r != VK_SUCCESS || info.pMappedData == nullptr)
                {
                    destroy();
                    return renderFailure<err::memory::GpuAllocationFailed>();
                }
                slot.mapped = info.pMappedData;
            }
            return {};
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return slots_.empty();
        }
        [[nodiscard]] std::uint32_t size() const noexcept
        {
            return static_cast<std::uint32_t>(slots_.size());
        }

        /// The slot index for the engine's REAL frame-in-flight (rule 1 above).
        [[nodiscard]] std::uint32_t slotIndexFor(std::uint32_t frame_index) const noexcept
        {
            return frame_index % static_cast<std::uint32_t>(slots_.size());
        }
        [[nodiscard]] Slot& slotAt(std::uint32_t index) noexcept
        {
            return slots_[index];
        }
        [[nodiscard]] const Slot& slotAt(std::uint32_t index) const noexcept
        {
            return slots_[index];
        }
        [[nodiscard]] Slot& slotFor(std::uint32_t frame_index) noexcept
        {
            return slots_[slotIndexFor(frame_index)];
        }

        /// Flush @p bytes of a slot's host-mapped range to the device.
        void flush(std::uint32_t index, VkDeviceSize bytes)
        {
            vmaFlushAllocation(allocator_, slots_[index].alloc, 0, bytes);
        }

        /// Runtime-detach path (rule 2 above): hand every live buffer to a frames-in-
        /// flight retire sink — `retire(VkBuffer, VmaAllocation)`, e.g. the deferred-
        /// destroy queue or a ContextView's retireBuffer — and null the handles so the
        /// destructor's destroy() becomes a no-op.
        template <class RetireFn> void retireInto(RetireFn&& retire)
        {
            for (auto& slot : slots_)
            {
                if (slot.buffer != VK_NULL_HANDLE)
                    retire(slot.buffer, slot.alloc);
                slot = {};
            }
        }

        /// Immediate-destruction fallback for the no-detach path ONLY (device idle at
        /// shutdown); a no-op after retireInto() nulled the handles.
        void destroy()
        {
            if (!allocator_)
                return;
            for (auto& slot : slots_)
            {
                if (slot.buffer != VK_NULL_HANDLE)
                    vmaDestroyBuffer(allocator_, slot.buffer, slot.alloc);
                slot = {};
            }
            slots_.clear();
        }

    private:
        VmaAllocator allocator_{nullptr};
        std::vector<Slot> slots_;
    };

} // namespace lux::render
