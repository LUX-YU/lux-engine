#pragma once

#include <lux/engine/render/core/RenderObjectTypes.hpp>
#include <lux/engine/render/resources/mesh/InstanceSlot.hpp>
#include <lux/engine/function/visibility.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::render
{

    class LUX_FUNCTION_PUBLIC InstanceSlotRegistry
    {
    public:
        static constexpr uint32_t kInvalidDensePos = ~0u;
        static constexpr uint32_t kInvalidObjectId = ~0u;

        struct CompactResult
        {
            std::vector<uint32_t> old_to_new;
            uint32_t live_count{0};
            bool compacted{false};
        };

        void init(uint32_t capacity);
        void shutdown();
        void resizeCapacity(uint32_t new_capacity);

        [[nodiscard]] bool needsGrowForAllocate() const noexcept;
        [[nodiscard]] InstanceSlot allocate();
        [[nodiscard]] RenderObjectHandle allocateObject();

        bool free(InstanceSlot slot);
        void freeObject(RenderObjectHandle handle);

        [[nodiscard]] bool isAlive(InstanceSlot slot) const noexcept;
        [[nodiscard]] bool isAlive(RenderObjectHandle handle) const noexcept;
        [[nodiscard]] uint32_t generation(InstanceSlot slot) const noexcept;
        [[nodiscard]] InstanceSlot resolveSlot(RenderObjectHandle handle) const noexcept;
        [[nodiscard]] RenderObjectHandle handleForSlot(InstanceSlot slot) const noexcept;
        InstanceSlot reserveSlot(uint32_t index);

        [[nodiscard]] CompactResult compact();

        [[nodiscard]] uint32_t slotCount() const noexcept { return slot_count_; }
        [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
        [[nodiscard]] uint32_t freeCount() const noexcept { return free_count_; }

        [[nodiscard]] std::span<const uint32_t> denseAliveSlots() const noexcept { return dense_alive_slots_; }
        [[nodiscard]] std::span<const RenderObjectHandle> denseAliveHandles() const noexcept { return dense_alive_handles_; }
        [[nodiscard]] std::span<const uint32_t> slotToObject() const noexcept { return slot_to_object_; }
        [[nodiscard]] std::span<const uint32_t> generations() const noexcept { return generations_; }

    private:
        [[nodiscard]] uint32_t claimFreeSlot();
        [[nodiscard]] uint32_t allocateObjectId();
        void releaseObjectId(uint32_t object_id);

        uint32_t slot_count_{0};
        uint32_t capacity_{0};

        std::vector<uint8_t> alive_;
        std::vector<uint64_t> free_bits_;
        uint32_t free_count_{0};
        uint32_t scan_hint_{0};

        std::vector<uint32_t> generations_;
        std::vector<uint32_t> slot_to_object_;
        std::vector<uint32_t> object_to_slot_;
        std::vector<uint32_t> object_generations_;
        std::vector<uint32_t> free_object_ids_;
        std::vector<uint32_t> dense_alive_slots_;
        std::vector<RenderObjectHandle> dense_alive_handles_;
        std::vector<uint32_t> slot_dense_pos_;
    };

} // namespace lux::render
