#include <lux/engine/render/resources/mesh/InstanceSlotRegistry.hpp>

#include <algorithm>
#include <bit>

namespace lux::render
{

void InstanceSlotRegistry::init(uint32_t capacity)
{
    shutdown();
    capacity_ = capacity;
    slot_count_ = 0;
    free_count_ = 0;
    scan_hint_ = 0;

    alive_.assign(capacity_, 0u);
    generations_.assign(capacity_, 0u);
    slot_to_object_.assign(capacity_, kInvalidObjectId);
    slot_dense_pos_.assign(capacity_, kInvalidDensePos);
    free_bits_.assign((capacity_ + 63u) / 64u, 0ull);
}

void InstanceSlotRegistry::shutdown()
{
    slot_count_ = 0;
    capacity_ = 0;
    free_count_ = 0;
    scan_hint_ = 0;

    alive_.clear();
    free_bits_.clear();
    generations_.clear();
    slot_to_object_.clear();
    object_to_slot_.clear();
    object_generations_.clear();
    free_object_ids_.clear();
    dense_alive_slots_.clear();
    dense_alive_handles_.clear();
    slot_dense_pos_.clear();
}

void InstanceSlotRegistry::resizeCapacity(uint32_t new_capacity)
{
    if (new_capacity <= capacity_)
        return;

    alive_.resize(new_capacity, 0u);
    generations_.resize(new_capacity, 0u);
    slot_to_object_.resize(new_capacity, kInvalidObjectId);
    slot_dense_pos_.resize(new_capacity, kInvalidDensePos);
    free_bits_.resize((new_capacity + 63u) / 64u, 0ull);
    capacity_ = new_capacity;
}

bool InstanceSlotRegistry::needsGrowForAllocate() const noexcept
{
    return free_count_ == 0u && slot_count_ >= capacity_;
}

InstanceSlot InstanceSlotRegistry::allocate()
{
    uint32_t slot = kInvalidDensePos;
    if (free_count_ > 0u)
    {
        slot = claimFreeSlot();
        if (slot >= slot_count_)
            slot_count_ = slot + 1u;
    }
    else
    {
        slot = slot_count_;
        if (slot >= capacity_)
            return InstanceSlot::invalid();
        ++slot_count_;
    }

    alive_[slot] = 1u;
    slot_to_object_[slot] = kInvalidObjectId;

    const uint32_t dense_pos = static_cast<uint32_t>(dense_alive_slots_.size());
    dense_alive_slots_.push_back(slot);
    dense_alive_handles_.push_back(RenderObjectHandle::invalid());
    slot_dense_pos_[slot] = dense_pos;
    return InstanceSlot{slot};
}

RenderObjectHandle InstanceSlotRegistry::allocateObject()
{
    const InstanceSlot slot = allocate();
    if (!slot)
        return RenderObjectHandle::invalid();

    const uint32_t object_id = allocateObjectId();
    if (object_id == kInvalidObjectId)
    {
        free(slot);
        return RenderObjectHandle::invalid();
    }

    slot_to_object_[slot.index] = object_id;
    object_to_slot_[object_id] = slot.index;

    if (slot.index < slot_dense_pos_.size())
    {
        const uint32_t dense_pos = slot_dense_pos_[slot.index];
        if (dense_pos != kInvalidDensePos && dense_pos < dense_alive_handles_.size())
            dense_alive_handles_[dense_pos] = RenderObjectHandle{object_id, object_generations_[object_id]};
    }

    return RenderObjectHandle{object_id, object_generations_[object_id]};
}

bool InstanceSlotRegistry::free(InstanceSlot slot)
{
    const uint32_t index = slot.index;
    if (index >= slot_count_ || !isAlive(slot))
        return false;

    const uint32_t object_id = (index < slot_to_object_.size()) ? slot_to_object_[index] : kInvalidObjectId;
    if (object_id != kInvalidObjectId)
    {
        releaseObjectId(object_id);
        slot_to_object_[index] = kInvalidObjectId;
    }

    if (index < slot_dense_pos_.size())
    {
        const uint32_t dense_pos = slot_dense_pos_[index];
        if (dense_pos != kInvalidDensePos && dense_pos < dense_alive_slots_.size())
        {
            const uint32_t last_pos = static_cast<uint32_t>(dense_alive_slots_.size() - 1u);
            if (dense_pos != last_pos)
            {
                const uint32_t moved_slot = dense_alive_slots_[last_pos];
                dense_alive_slots_[dense_pos] = moved_slot;
                dense_alive_handles_[dense_pos] = dense_alive_handles_[last_pos];
                slot_dense_pos_[moved_slot] = dense_pos;
            }
            dense_alive_slots_.pop_back();
            dense_alive_handles_.pop_back();
            slot_dense_pos_[index] = kInvalidDensePos;
        }
    }

    alive_[index] = 0u;
    ++generations_[index];

    const uint32_t word = index / 64u;
    const uint32_t bit = index % 64u;
    free_bits_[word] |= (1ull << bit);
    ++free_count_;
    return true;
}

void InstanceSlotRegistry::freeObject(RenderObjectHandle handle)
{
    free(resolveSlot(handle));
}

bool InstanceSlotRegistry::isAlive(InstanceSlot slot) const noexcept
{
    return slot.index < slot_count_ && slot.index < alive_.size() && alive_[slot.index] != 0u;
}

bool InstanceSlotRegistry::isAlive(RenderObjectHandle handle) const noexcept
{
    return isAlive(resolveSlot(handle));
}

uint32_t InstanceSlotRegistry::generation(InstanceSlot slot) const noexcept
{
    if (slot.index >= slot_to_object_.size())
        return 0u;
    const uint32_t object_id = slot_to_object_[slot.index];
    if (object_id == kInvalidObjectId || object_id >= object_generations_.size())
        return 0u;
    return object_generations_[object_id];
}

InstanceSlot InstanceSlotRegistry::resolveSlot(RenderObjectHandle handle) const noexcept
{
    if (handle.index >= object_to_slot_.size() || handle.index >= object_generations_.size())
        return InstanceSlot::invalid();
    if (object_generations_[handle.index] != handle.gen)
        return InstanceSlot::invalid();

    const uint32_t slot = object_to_slot_[handle.index];
    if (slot == kInvalidDensePos || slot >= slot_count_)
        return InstanceSlot::invalid();
    if (slot >= slot_to_object_.size() || slot_to_object_[slot] != handle.index)
        return InstanceSlot::invalid();
    if (!isAlive(InstanceSlot{slot}))
        return InstanceSlot::invalid();
    return InstanceSlot{slot};
}

RenderObjectHandle InstanceSlotRegistry::handleForSlot(InstanceSlot slot) const noexcept
{
    if (!isAlive(slot) || slot.index >= slot_to_object_.size())
        return RenderObjectHandle::invalid();
    const uint32_t object_id = slot_to_object_[slot.index];
    if (object_id == kInvalidObjectId || object_id >= object_generations_.size())
        return RenderObjectHandle::invalid();
    return RenderObjectHandle{object_id, object_generations_[object_id]};
}

InstanceSlot InstanceSlotRegistry::reserveSlot(uint32_t index)
{
    if (index >= capacity_)
        return InstanceSlot::invalid();
    if (index >= slot_count_)
        slot_count_ = index + 1u;

    const uint32_t word = index / 64u;
    const uint32_t bit = index % 64u;
    if (free_bits_[word] & (1ull << bit))
    {
        free_bits_[word] &= ~(1ull << bit);
        --free_count_;
    }

    return InstanceSlot{index};
}

InstanceSlotRegistry::CompactResult InstanceSlotRegistry::compact()
{
    CompactResult result{};
    result.live_count = static_cast<uint32_t>(dense_alive_slots_.size());
    if (result.live_count == 0u)
    {
        slot_count_ = 0u;
        std::fill(alive_.begin(), alive_.end(), 0u);
        std::fill(slot_to_object_.begin(), slot_to_object_.end(), kInvalidObjectId);
        std::fill(slot_dense_pos_.begin(), slot_dense_pos_.end(), kInvalidDensePos);
        dense_alive_slots_.clear();
        dense_alive_handles_.clear();

        free_bits_.assign((capacity_ + 63u) / 64u, 0ull);
        free_count_ = 0u;
        for (uint32_t slot = 0; slot < capacity_; ++slot)
        {
            const uint32_t word = slot / 64u;
            const uint32_t bit = slot % 64u;
            free_bits_[word] |= (1ull << bit);
            ++free_count_;
        }
        result.compacted = true;
        return result;
    }

    result.old_to_new.assign(slot_count_, kInvalidDensePos);
    for (uint32_t new_slot = 0; new_slot < result.live_count; ++new_slot)
    {
        const uint32_t old_slot = dense_alive_slots_[new_slot];
        result.old_to_new[old_slot] = new_slot;
    }

    std::vector<uint8_t> compact_alive(capacity_, 0u);
    std::vector<uint32_t> compact_generations = generations_;
    std::vector<uint32_t> compact_slot_to_object(capacity_, kInvalidObjectId);
    std::vector<uint32_t> compact_slot_dense_pos(capacity_, kInvalidDensePos);

    for (uint32_t old_slot = 0; old_slot < slot_count_; ++old_slot)
    {
        const uint32_t new_slot = result.old_to_new[old_slot];
        if (new_slot == kInvalidDensePos)
            continue;
        compact_alive[new_slot] = 1u;
        compact_generations[new_slot] = generations_[old_slot];
        compact_slot_to_object[new_slot] = slot_to_object_[old_slot];
        compact_slot_dense_pos[new_slot] = new_slot;

        const uint32_t object_id = slot_to_object_[old_slot];
        if (object_id != kInvalidObjectId && object_id < object_to_slot_.size())
            object_to_slot_[object_id] = new_slot;
    }

    alive_ = std::move(compact_alive);
    generations_ = std::move(compact_generations);
    slot_to_object_ = std::move(compact_slot_to_object);
    slot_dense_pos_ = std::move(compact_slot_dense_pos);

    dense_alive_slots_.resize(result.live_count);
    dense_alive_handles_.resize(result.live_count);
    for (uint32_t slot = 0; slot < result.live_count; ++slot)
    {
        dense_alive_slots_[slot] = slot;
        const uint32_t object_id = slot_to_object_[slot];
        if (object_id != kInvalidObjectId && object_id < object_generations_.size())
            dense_alive_handles_[slot] = RenderObjectHandle{object_id, object_generations_[object_id]};
        else
            dense_alive_handles_[slot] = RenderObjectHandle::invalid();
    }

    slot_count_ = result.live_count;
    free_bits_.assign((capacity_ + 63u) / 64u, 0ull);
    free_count_ = 0u;
    for (uint32_t slot = slot_count_; slot < capacity_; ++slot)
    {
        const uint32_t word = slot / 64u;
        const uint32_t bit = slot % 64u;
        free_bits_[word] |= (1ull << bit);
        ++free_count_;
    }
    scan_hint_ = slot_count_ / 64u;

    result.compacted = true;
    return result;
}

uint32_t InstanceSlotRegistry::claimFreeSlot()
{
    const uint32_t word_count = static_cast<uint32_t>(free_bits_.size());
    for (uint32_t attempt = 0; attempt < word_count; ++attempt)
    {
        const uint32_t word = (scan_hint_ + attempt) % word_count;
        uint64_t bits = free_bits_[word];
        if (bits == 0u)
            continue;

        const uint32_t bit = static_cast<uint32_t>(std::countr_zero(bits));
        free_bits_[word] = bits & (bits - 1u);
        --free_count_;
        scan_hint_ = word;
        return word * 64u + bit;
    }
    return slot_count_++;
}

uint32_t InstanceSlotRegistry::allocateObjectId()
{
    if (!free_object_ids_.empty())
    {
        const uint32_t object_id = free_object_ids_.back();
        free_object_ids_.pop_back();
        return object_id;
    }

    const uint32_t object_id = static_cast<uint32_t>(object_to_slot_.size());
    object_to_slot_.push_back(kInvalidDensePos);
    object_generations_.push_back(0u);
    return object_id;
}

void InstanceSlotRegistry::releaseObjectId(uint32_t object_id)
{
    if (object_id >= object_to_slot_.size() || object_id >= object_generations_.size())
        return;
    object_to_slot_[object_id] = kInvalidDensePos;
    ++object_generations_[object_id];
    free_object_ids_.push_back(object_id);
}

} // namespace lux::render

