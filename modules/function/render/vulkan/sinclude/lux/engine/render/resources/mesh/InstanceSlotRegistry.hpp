#pragma once

#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>
#include <lux/engine/render/resources/mesh/InstanceSlot.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>
#include <vector>

namespace lux::render
{
    /// Stable render-object slot allocator. RenderObjectHandle::index is the
    /// physical sparse slot itself; there is no movable object-id indirection.
    class LUX_FUNCTION_PUBLIC InstanceSlotRegistry
    {
    public:
        static constexpr std::uint32_t kInvalidDensePos = ~0u;

        /// Generation zero is never live. Wrapping a slot from UINT32_MAX to
        /// zero permanently retires it instead of making an ancient handle
        /// valid again.
        [[nodiscard]] static constexpr std::uint32_t nextGeneration(
            std::uint32_t generation) noexcept
        {
            return generation + 1u;
        }

        void init(std::uint32_t capacity);
        void shutdown();
        void resizeCapacity(std::uint32_t new_capacity);

        [[nodiscard]] bool needsGrowForAllocate() const noexcept;
        [[nodiscard]] InstanceSlot allocate();
        [[nodiscard]] RenderObjectHandle allocateObject();

        bool free(InstanceSlot slot);
        void freeObject(RenderObjectHandle handle);

        [[nodiscard]] bool isAlive(InstanceSlot slot) const noexcept;
        [[nodiscard]] bool isAlive(RenderObjectHandle handle) const noexcept;
        [[nodiscard]] std::uint32_t generation(
            InstanceSlot slot) const noexcept;
        [[nodiscard]] InstanceSlot resolveSlot(
            RenderObjectHandle handle) const noexcept;
        [[nodiscard]] RenderObjectHandle handleForSlot(
            InstanceSlot slot) const noexcept;

        [[nodiscard]] std::uint32_t slotCount() const noexcept
        {
            return slot_count_;
        }
        [[nodiscard]] std::uint32_t capacity() const noexcept
        {
            return capacity_;
        }
        [[nodiscard]] std::uint32_t freeCount() const noexcept
        {
            return static_cast<std::uint32_t>(free_slots_.size());
        }
        [[nodiscard]] std::uint32_t retiredCount() const noexcept
        {
            return retired_count_;
        }

        [[nodiscard]] std::span<const std::uint32_t> denseAliveSlots()
            const noexcept
        {
            return dense_alive_slots_;
        }
        [[nodiscard]] std::uint32_t densePosition(
            InstanceSlot slot) const noexcept
        {
            return slot.index < slot_dense_pos_.size()
                ? slot_dense_pos_[slot.index]
                : kInvalidDensePos;
        }
        [[nodiscard]] std::span<const std::uint32_t> generations()
            const noexcept
        {
            return generations_;
        }

    private:
        std::uint32_t slot_count_{0u};
        std::uint32_t capacity_{0u};
        std::uint32_t retired_count_{0u};

        std::vector<std::uint8_t> alive_;
        std::vector<std::uint32_t> generations_;
        std::vector<std::uint32_t> free_slots_;
        std::vector<std::uint32_t> dense_alive_slots_;
        std::vector<std::uint32_t> slot_dense_pos_;
    };
} // namespace lux::render
