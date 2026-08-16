#pragma once
/**
 * @file ChainedArenaAllocator.hpp
 * @brief Multi-segment arena allocator that automatically grows by adding new
 *        segments when the current one is full.
 *
 * Each segment is backed by its own VkBuffer (allocated externally by the
 * MeshResources layer).  The allocator only manages byte ranges; it does
 * not own any Vulkan resources.
 *
 * Thread safety: NOT thread-safe.  External synchronization is required.
 */

#include <lux/engine/render/gpu/memory/ArenaAllocator.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace lux::render
{
    /// Result of a chained arena allocation.
    struct SegmentedAllocation
    {
        uint16_t segment_index{0};   ///< Which segment this allocation lives in
        uint64_t offset{0};          ///< Byte offset within the segment
        uint64_t size{0};            ///< Size of the allocation in bytes
        VmaVirtualAllocation handle{VK_NULL_HANDLE}; ///< VMA handle (needed for free)
        [[nodiscard]] bool valid() const { return size > 0; }
    };

    /**
     * @brief Growable multi-segment arena allocator.
     *
     * Allocations try the most recent segment first, then fall back to earlier
     * segments (for reuse of freed space), and finally create a new segment
     * if no existing segment can satisfy the request.
     *
     * The caller must provide a segment creation callback that allocates a
     * VkBuffer of the requested size and returns true on success.
     */
    class ChainedArenaAllocator
    {
    public:
        /// Zero means that allocation is bounded only by the uint16 segment
        /// protocol and the external CapacityPlan/VRAM admission.
        static constexpr uint16_t kDefaultMaxSegments = 0;
        static constexpr uint16_t kInvalidSegment     = 0xFFFF;

        ChainedArenaAllocator() = default;

        /**
         * @brief Initialize with the first (seed) segment.
         * @param seed_capacity  Size of the first segment in bytes.
         * @param max_segments   Maximum number of segments (default 16, 0 = unlimited).
         */
        explicit ChainedArenaAllocator(uint64_t seed_capacity,
                                       uint16_t max_segments = kDefaultMaxSegments)
            : max_segments_(max_segments)
        {
            if (seed_capacity > 0)
                addSegment(seed_capacity);
        }

        /// Number of active segments.
        [[nodiscard]] uint16_t segmentCount() const { return static_cast<uint16_t>(segments_.size()); }

        /// Capacity of a specific segment.
        [[nodiscard]] uint64_t segmentCapacity(uint16_t idx) const
        {
            return idx < segments_.size() ? segments_[idx]->totalCapacity() : 0;
        }

        /// Total used bytes across all segments.
        [[nodiscard]] uint64_t totalUsedBytes() const
        {
            uint64_t total = 0;
            for (auto& s : segments_) total += s->usedBytes();
            return total;
        }

        /// Total capacity across all segments.
        [[nodiscard]] uint64_t totalCapacity() const
        {
            uint64_t total = 0;
            for (auto& s : segments_) total += s->totalCapacity();
            return total;
        }

        [[nodiscard]] uint64_t largestFreeBlock() const
        {
            uint64_t largest = 0u;
            for (const auto& segment : segments_)
                largest = std::max(largest, segment->largestFreeBlock());
            return largest;
        }

        [[nodiscard]] float fragmentationRatio() const
        {
            const auto capacity = totalCapacity();
            const auto used = totalUsedBytes();
            const auto free = capacity > used ? capacity - used : 0u;
            if (free == 0u)
                return 0.0f;
            const auto largest = largestFreeBlock();
            return largest >= free
                ? 0.0f
                : 1.0f - static_cast<float>(largest) /
                    static_cast<float>(free);
        }

        /**
         * @brief Allocate a region.
         *
         * Attempts the most recent segment first, falls back to earlier ones,
         * and returns an invalid allocation if all segments are full.
         * The caller should call addSegment() + retry if this returns invalid.
         */
        [[nodiscard]] SegmentedAllocation allocate(uint64_t size, uint64_t alignment = 1)
        {
            if (size == 0 || segments_.empty()) return {};

            // Try most-recent segment first (hot path).
            {
                auto& last = *segments_.back();
                auto alloc = last.allocate(size, alignment);
                if (alloc.valid())
                    return { static_cast<uint16_t>(segments_.size() - 1), alloc.offset, alloc.size, alloc.handle };
            }

            // Fall back to earlier segments (reuse freed space).
            for (uint16_t i = 0; i < segments_.size() - 1; ++i)
            {
                auto alloc = segments_[i]->allocate(size, alignment);
                if (alloc.valid())
                    return { i, alloc.offset, alloc.size, alloc.handle };
            }

            return {}; // all segments full
        }

        [[nodiscard]] bool canAllocate(
            uint64_t size,
            uint64_t alignment = 1)
        {
            if (size == 0u)
                return true;
            if (segments_.empty())
                return false;
            if (segments_.back()->canAllocate(size, alignment))
                return true;
            for (uint16_t index = 0u; index + 1u < segments_.size(); ++index)
            {
                if (segments_[index]->canAllocate(size, alignment))
                    return true;
            }
            return false;
        }

        /**
         * @brief Free a previously allocated region.
         */
        void free(const SegmentedAllocation& alloc)
        {
            if (!alloc.valid() || alloc.segment_index >= segments_.size()) return;
            segments_[alloc.segment_index]->free({ alloc.offset, alloc.size, alloc.handle });
        }

        /**
         * @brief Add a new segment with the given capacity.
         * @return The index of the new segment, or kInvalidSegment on failure.
         */
        uint16_t addSegment(uint64_t capacity)
        {
            if (capacity == 0) return kInvalidSegment;
            if ((max_segments_ > 0 && segments_.size() >= max_segments_) ||
                segments_.size() >= kInvalidSegment)
            {
                return kInvalidSegment;
            }
            auto idx = static_cast<uint16_t>(segments_.size());
            segments_.push_back(std::make_unique<ArenaAllocator>(capacity));
            return idx;
        }

        /// Roll back the most recently added segment before it is published to
        /// users. Only an empty non-seed segment may be removed.
        [[nodiscard]] bool removeLastEmptySegment()
        {
            if (segments_.size() <= 1u || segments_.back()->usedBytes() != 0u)
                return false;
            segments_.pop_back();
            return true;
        }

        /// Access the underlying ArenaAllocator for a segment (for defrag queries).
        [[nodiscard]] ArenaAllocator* segment(uint16_t idx)
        {
            return idx < segments_.size() ? segments_[idx].get() : nullptr;
        }

        /// Reset all segments.
        void clear() { segments_.clear(); }

    private:
        std::vector<std::unique_ptr<ArenaAllocator>> segments_;
        uint16_t max_segments_{kDefaultMaxSegments}; ///< 0 = unlimited
    };

} // namespace lux::render
