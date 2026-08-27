#include <lux/engine/render/resources/mesh/InstanceSlotRegistry.hpp>

#include <algorithm>

namespace lux::render
{
    void InstanceSlotRegistry::init(std::uint32_t capacity)
    {
        shutdown();
        capacity_ = capacity;
        alive_.assign(capacity_, 0u);
        generations_.assign(capacity_, 1u);
        slot_dense_pos_.assign(capacity_, kInvalidDensePos);
    }

    void InstanceSlotRegistry::shutdown()
    {
        slot_count_ = 0u;
        capacity_ = 0u;
        retired_count_ = 0u;
        alive_.clear();
        generations_.clear();
        free_slots_.clear();
        dense_alive_slots_.clear();
        slot_dense_pos_.clear();
    }

    void InstanceSlotRegistry::resizeCapacity(std::uint32_t new_capacity)
    {
        if (new_capacity <= capacity_)
            return;
        alive_.resize(new_capacity, 0u);
        generations_.resize(new_capacity, 1u);
        slot_dense_pos_.resize(new_capacity, kInvalidDensePos);
        capacity_ = new_capacity;
    }

    bool InstanceSlotRegistry::needsGrowForAllocate() const noexcept
    {
        return free_slots_.empty() && slot_count_ >= capacity_;
    }

    InstanceSlot InstanceSlotRegistry::allocate()
    {
        std::uint32_t index = kInvalidDensePos;
        if (!free_slots_.empty())
        {
            index = free_slots_.back();
            free_slots_.pop_back();
        }
        else
        {
            if (slot_count_ >= capacity_ || slot_count_ == kInvalidDensePos)
                return InstanceSlot::invalid();
            index = slot_count_++;
        }

        if (index >= generations_.size() || generations_[index] == 0u)
            return InstanceSlot::invalid();

        alive_[index] = 1u;
        const auto dense_position = static_cast<std::uint32_t>(dense_alive_slots_.size());
        dense_alive_slots_.push_back(index);
        slot_dense_pos_[index] = dense_position;
        return InstanceSlot{index};
    }

    RenderObjectHandle InstanceSlotRegistry::allocateObject()
    {
        const auto slot = allocate();
        return slot ? handleForSlot(slot) : RenderObjectHandle::invalid();
    }

    bool InstanceSlotRegistry::free(InstanceSlot slot)
    {
        if (!isAlive(slot))
            return false;

        const auto index = slot.index;
        const auto dense_position = slot_dense_pos_[index];
        const auto last_position = static_cast<std::uint32_t>(dense_alive_slots_.size() - 1u);
        if (dense_position != last_position)
        {
            const auto moved_slot = dense_alive_slots_[last_position];
            dense_alive_slots_[dense_position] = moved_slot;
            slot_dense_pos_[moved_slot] = dense_position;
        }
        dense_alive_slots_.pop_back();
        slot_dense_pos_[index] = kInvalidDensePos;
        alive_[index] = 0u;

        generations_[index] = nextGeneration(generations_[index]);
        if (generations_[index] == 0u)
        {
            ++retired_count_;
            return true;
        }
        free_slots_.push_back(index);
        return true;
    }

    void InstanceSlotRegistry::freeObject(RenderObjectHandle handle)
    {
        const auto slot = resolveSlot(handle);
        if (slot)
            free(slot);
    }

    bool InstanceSlotRegistry::isAlive(InstanceSlot slot) const noexcept
    {
        return slot.index < slot_count_ && slot.index < alive_.size() && alive_[slot.index] != 0u;
    }

    bool InstanceSlotRegistry::isAlive(RenderObjectHandle handle) const noexcept
    {
        return handle.index != kInvalidDensePos && handle.index < slot_count_ && handle.index < generations_.size() &&
               alive_[handle.index] != 0u && generations_[handle.index] == handle.gen;
    }

    std::uint32_t InstanceSlotRegistry::generation(InstanceSlot slot) const noexcept
    {
        return isAlive(slot) ? generations_[slot.index] : 0u;
    }

    InstanceSlot InstanceSlotRegistry::resolveSlot(RenderObjectHandle handle) const noexcept
    {
        return isAlive(handle) ? InstanceSlot{handle.index} : InstanceSlot::invalid();
    }

    RenderObjectHandle InstanceSlotRegistry::handleForSlot(InstanceSlot slot) const noexcept
    {
        return isAlive(slot) ? RenderObjectHandle{slot.index, generations_[slot.index]} : RenderObjectHandle::invalid();
    }
} // namespace lux::render
