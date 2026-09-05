#pragma once

#include <lux/cxx/compile_time/expected.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace lux::simulation::script::detail
{
    enum class EBoundedFrameStorageError : std::uint8_t
    {
        INVALID_CONFIGURATION,
        CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
    };

    class BoundedFrameStorage final
    {
    public:
        struct Allocation final
        {
            void* data{};
            std::uint32_t slot{(std::numeric_limits<std::uint32_t>::max)()};
            std::uint32_t generation{};
            std::size_t size{};

            [[nodiscard]] explicit operator bool() const noexcept
            {
                return data != nullptr && slot != (std::numeric_limits<std::uint32_t>::max)();
            }
        };

        struct Stats final
        {
            std::size_t storage_bytes{};
            std::size_t active_frames{};
            std::size_t frame_high_water{};
            std::size_t capacity_failures{};
        };

        [[nodiscard]] static lux::cxx::expected<BoundedFrameStorage, EBoundedFrameStorageError> create(
            std::size_t storage_bytes,
            std::size_t frame_capacity,
            std::size_t storage_alignment
        ) noexcept
        {
            const bool is_invalid_alignment = storage_alignment < alignof(std::max_align_t) ||
                (storage_alignment & (storage_alignment - 1U)) != 0U;
            const bool is_invalid_capacity = storage_bytes == 0U || frame_capacity == 0U ||
                storage_bytes > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) ||
                frame_capacity > ((std::numeric_limits<std::uint32_t>::max)() - 1U) / 2U ||
                frame_capacity > ((std::numeric_limits<std::size_t>::max)() - 1U) / 2U;
            if (is_invalid_alignment || is_invalid_capacity)
                return lux::cxx::unexpected<EBoundedFrameStorageError>(
                    EBoundedFrameStorageError::INVALID_CONFIGURATION
                );

            void* arena = ::operator new(storage_bytes, std::align_val_t{storage_alignment}, std::nothrow);
            if (arena == nullptr)
                return lux::cxx::unexpected<EBoundedFrameStorageError>(
                    EBoundedFrameStorageError::ALLOCATION_FAILURE
                );
            BoundedFrameStorage result;
            result.arena_ = arena;
            result.storage_alignment_ = storage_alignment;
            try
            {
                result.storage_bytes_ = storage_bytes;
                result.frame_capacity_ = frame_capacity;
                result.blocks_.resize(frame_capacity * 2U + 1U);
                result.free_metadata_.reserve(result.blocks_.size() - 1U);
                for (std::size_t index = result.blocks_.size(); index > 1U; --index)
                    result.free_metadata_.push_back(static_cast<std::uint32_t>(index - 1U));
                result.blocks_[0] = Block{0U, storage_bytes, invalidSlot(), invalidSlot(), 1U, true, true};
                result.head_ = 0U;
                result.free_head_ = 0U;
                return result;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected<EBoundedFrameStorageError>(
                    EBoundedFrameStorageError::ALLOCATION_FAILURE
                );
            }
        }

        BoundedFrameStorage() noexcept = default;
        BoundedFrameStorage(const BoundedFrameStorage&) = delete;
        BoundedFrameStorage& operator=(const BoundedFrameStorage&) = delete;

        BoundedFrameStorage(BoundedFrameStorage&& other) noexcept
            : arena_(std::exchange(other.arena_, nullptr)),
              storage_bytes_(std::exchange(other.storage_bytes_, 0U)),
              storage_alignment_(std::exchange(other.storage_alignment_, alignof(std::max_align_t))),
              frame_capacity_(std::exchange(other.frame_capacity_, 0U)),
              blocks_(std::move(other.blocks_)),
              free_metadata_(std::move(other.free_metadata_)),
              head_(std::exchange(other.head_, invalidSlot())),
              free_head_(std::exchange(other.free_head_, invalidSlot())),
              active_frames_(std::exchange(other.active_frames_, 0U)),
              frame_high_water_(std::exchange(other.frame_high_water_, 0U)),
              capacity_failures_(std::exchange(other.capacity_failures_, 0U))
        {
        }

        BoundedFrameStorage& operator=(BoundedFrameStorage&& other) noexcept
        {
            if (this == std::addressof(other))
                return *this;
            reset();
            arena_ = std::exchange(other.arena_, nullptr);
            storage_bytes_ = std::exchange(other.storage_bytes_, 0U);
            storage_alignment_ = std::exchange(other.storage_alignment_, alignof(std::max_align_t));
            frame_capacity_ = std::exchange(other.frame_capacity_, 0U);
            blocks_ = std::move(other.blocks_);
            free_metadata_ = std::move(other.free_metadata_);
            head_ = std::exchange(other.head_, invalidSlot());
            free_head_ = std::exchange(other.free_head_, invalidSlot());
            active_frames_ = std::exchange(other.active_frames_, 0U);
            frame_high_water_ = std::exchange(other.frame_high_water_, 0U);
            capacity_failures_ = std::exchange(other.capacity_failures_, 0U);
            return *this;
        }

        ~BoundedFrameStorage()
        {
            reset();
        }

        [[nodiscard]] lux::cxx::expected<Allocation, EBoundedFrameStorageError> acquire(
            std::size_t size,
            std::size_t alignment
        ) noexcept
        {
            const bool is_invalid_alignment = alignment == 0U || alignment > storage_alignment_ ||
                (alignment & (alignment - 1U)) != 0U;
            if (arena_ == nullptr || size == 0U || is_invalid_alignment || active_frames_ >= frame_capacity_)
                return fail(EBoundedFrameStorageError::CAPACITY_EXCEEDED);

            auto current = free_head_;
            while (current != invalidSlot())
            {
                auto& block = blocks_[current];
                const auto aligned = alignUp(block.offset, alignment);
                const bool offset_overflow = aligned < block.offset;
                const bool size_overflow = offset_overflow || aligned > storage_bytes_ ||
                    size > storage_bytes_ - aligned;
                const bool fits = !size_overflow && aligned + size <= block.offset + block.size;
                if (!fits)
                {
                    current = block.free_next;
                    continue;
                }

                const auto prefix = aligned - block.offset;
                const auto suffix = block.offset + block.size - aligned - size;
                if (block.generation == (std::numeric_limits<std::uint32_t>::max)())
                    return fail(EBoundedFrameStorageError::CAPACITY_EXCEEDED);
                const auto metadata_needed = static_cast<std::size_t>(prefix != 0U) +
                    static_cast<std::size_t>(suffix != 0U);
                if (free_metadata_.size() < metadata_needed)
                    return fail(EBoundedFrameStorageError::CAPACITY_EXCEEDED);

                const auto previous = block.previous;
                const auto next = block.next;
                unlinkFree(current);
                if (prefix != 0U)
                {
                    const auto prefix_slot = takeMetadata();
                    blocks_[prefix_slot] = Block{
                        block.offset, prefix, previous, current, blocks_[prefix_slot].generation, true, true
                    };
                    linkFree(prefix_slot);
                    if (previous != invalidSlot())
                        blocks_[previous].next = prefix_slot;
                    else
                        head_ = prefix_slot;
                    block.previous = prefix_slot;
                }
                if (suffix != 0U)
                {
                    const auto suffix_slot = takeMetadata();
                    blocks_[suffix_slot] = Block{
                        aligned + size, suffix, current, next, blocks_[suffix_slot].generation, true, true
                    };
                    linkFree(suffix_slot);
                    if (next != invalidSlot())
                        blocks_[next].previous = suffix_slot;
                    block.next = suffix_slot;
                }
                else
                {
                    block.next = next;
                    if (next != invalidSlot())
                        blocks_[next].previous = current;
                }
                block.offset = aligned;
                block.size = size;
                block.free = false;
                block.used = true;
                ++block.generation;
                ++active_frames_;
                frame_high_water_ = (std::max)(frame_high_water_, active_frames_);
                return Allocation{
                    static_cast<std::byte*>(arena_) + aligned,
                    current,
                    block.generation,
                    size
                };
            }
            return fail(EBoundedFrameStorageError::CAPACITY_EXCEEDED);
        }

        [[nodiscard]] bool release(Allocation allocation) noexcept
        {
            if (!allocation || allocation.slot >= blocks_.size())
                return false;
            auto& block = blocks_[allocation.slot];
            if (!block.used || block.free || block.generation != allocation.generation ||
                block.size != allocation.size || static_cast<std::byte*>(arena_) + block.offset != allocation.data)
            {
                return false;
            }
            block.free = true;
            linkFree(allocation.slot);
            --active_frames_;
            auto slot = allocation.slot;
            if (block.previous != invalidSlot() && blocks_[block.previous].free)
                slot = merge(block.previous, slot);
            if (blocks_[slot].next != invalidSlot() && blocks_[blocks_[slot].next].free)
                static_cast<void>(merge(slot, blocks_[slot].next));
            return true;
        }

        [[nodiscard]] Stats stats() const noexcept
        {
            return {storage_bytes_, active_frames_, frame_high_water_, capacity_failures_};
        }

    private:
        struct Block final
        {
            std::size_t offset{};
            std::size_t size{};
            std::uint32_t previous{invalidSlot()};
            std::uint32_t next{invalidSlot()};
            std::uint32_t generation{};
            bool free{};
            bool used{};
            std::uint32_t free_previous{invalidSlot()};
            std::uint32_t free_next{invalidSlot()};
        };

        [[nodiscard]] static constexpr std::uint32_t invalidSlot() noexcept
        {
            return (std::numeric_limits<std::uint32_t>::max)();
        }

        [[nodiscard]] static std::size_t alignUp(std::size_t value, std::size_t alignment) noexcept
        {
            const auto padding = alignment - 1U;
            if (value > (std::numeric_limits<std::size_t>::max)() - padding)
                return 0U;
            return (value + padding) & ~padding;
        }

        [[nodiscard]] std::uint32_t takeMetadata() noexcept
        {
            const auto result = free_metadata_.back();
            free_metadata_.pop_back();
            return result;
        }

        void linkFree(std::uint32_t slot) noexcept
        {
            auto& block = blocks_[slot];
            block.free_previous = invalidSlot();
            block.free_next = free_head_;
            if (free_head_ != invalidSlot())
                blocks_[free_head_].free_previous = slot;
            free_head_ = slot;
        }

        void unlinkFree(std::uint32_t slot) noexcept
        {
            auto& block = blocks_[slot];
            if (block.free_previous != invalidSlot())
                blocks_[block.free_previous].free_next = block.free_next;
            else
                free_head_ = block.free_next;
            if (block.free_next != invalidSlot())
                blocks_[block.free_next].free_previous = block.free_previous;
            block.free_previous = invalidSlot();
            block.free_next = invalidSlot();
        }

        void returnMetadata(std::uint32_t slot) noexcept
        {
            const auto generation = blocks_[slot].generation;
            blocks_[slot] = {};
            blocks_[slot].generation = generation;
            free_metadata_.push_back(slot);
        }

        [[nodiscard]] std::uint32_t merge(std::uint32_t left, std::uint32_t right) noexcept
        {
            auto& left_block = blocks_[left];
            auto& right_block = blocks_[right];
            unlinkFree(left);
            unlinkFree(right);
            left_block.size += right_block.size;
            left_block.next = right_block.next;
            if (right_block.next != invalidSlot())
                blocks_[right_block.next].previous = left;
            returnMetadata(right);
            linkFree(left);
            return left;
        }

        [[nodiscard]] lux::cxx::expected<Allocation, EBoundedFrameStorageError> fail(
            EBoundedFrameStorageError error
        ) noexcept
        {
            ++capacity_failures_;
            return lux::cxx::unexpected<EBoundedFrameStorageError>(error);
        }

        void reset() noexcept
        {
            if (arena_ != nullptr)
                ::operator delete(arena_, std::align_val_t{storage_alignment_});
            arena_ = nullptr;
            storage_bytes_ = 0U;
            active_frames_ = 0U;
            head_ = invalidSlot();
            free_head_ = invalidSlot();
        }

        void* arena_{};
        std::size_t storage_bytes_{};
        std::size_t storage_alignment_{alignof(std::max_align_t)};
        std::size_t frame_capacity_{};
        std::vector<Block> blocks_;
        std::vector<std::uint32_t> free_metadata_;
        std::uint32_t head_{invalidSlot()};
        std::uint32_t free_head_{invalidSlot()};
        std::size_t active_frames_{};
        std::size_t frame_high_water_{};
        std::size_t capacity_failures_{};
    };
}
