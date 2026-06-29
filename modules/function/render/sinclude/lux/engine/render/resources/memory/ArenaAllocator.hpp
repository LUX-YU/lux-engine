#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/core/VmaFwd.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lux::render
{
    /**
     * @brief Arena sub-allocator backed by VMA Virtual Allocator (TLSF).
     *
     * Uses VmaVirtualBlock for O(1) allocate/free with automatic coalescing.
     * Maintains a parallel live-allocation list so planDefragmentation() can
     * still produce the GPU copy-ops that MeshResources needs.
     *
     * Thread safety: NOT thread-safe.  External synchronization is required.
     */
    class LUX_FUNCTION_PUBLIC ArenaAllocator
    {
    public:
        /// @brief A single sub-allocation within the arena
        struct Allocation
        {
            uint64_t offset{ 0 };
            uint64_t size{ 0 };
            VmaVirtualAllocation handle{ VK_NULL_HANDLE }; ///< VMA handle for free()
            bool valid() const { return size > 0; }
        };

        /// @brief A copy operation for defragmentation
        struct CopyOp
        {
            uint64_t src_offset;
            uint64_t dst_offset;
            uint64_t size;
        };

        explicit ArenaAllocator(uint64_t total_capacity);
        ArenaAllocator();
        ~ArenaAllocator();

        // Non-copyable, movable
        ArenaAllocator(const ArenaAllocator&) = delete;
        ArenaAllocator& operator=(const ArenaAllocator&) = delete;

        ArenaAllocator(ArenaAllocator&& o) noexcept;
        ArenaAllocator& operator=(ArenaAllocator&& o) noexcept;

        /**
         * @brief Allocate a region with given size and alignment.
         * @return A valid Allocation on success, or an Allocation with size=0 on failure.
         */
        [[nodiscard]] Allocation allocate(uint64_t size, uint64_t alignment = 1);

        /**
         * @brief Free a previously allocated region.
         */
        void free(const Allocation& alloc);

        /**
         * @brief Fragmentation ratio in [0, 1].
         *
         * 0 = no fragmentation.  Higher = more fragmented.
         */
        float fragmentationRatio() const;

        /**
         * @brief Plan a defragmentation pass.
         *
         * Returns a list of copy operations that move all live allocations
         * toward offset 0, eliminating gaps.  The caller is responsible for:
         *   1. Executing the copies (vkCmdCopyBuffer)
         *   2. Calling applyDefragmentation() to update internal bookkeeping
         *   3. Updating any external handle -> offset mappings
         *
         * @return Vector of CopyOps sorted by destination offset.
         */
        std::vector<CopyOp> planDefragmentation() const;

        /**
         * @brief Apply defragmentation result - rebuild the virtual block
         *        with all live data compacted at the front.
         */
        void applyDefragmentation(const std::vector<CopyOp>& ops);

        // ========== Query ==========

        uint64_t totalCapacity() const;
        uint64_t usedBytes() const;
        uint64_t freeBytes() const;
        size_t freeBlockCount() const;

        /// @brief Check if a specific region is entirely within free space
        bool isFree(uint64_t offset, uint64_t size) const;

        /// @brief Look up the VMA handle for the live allocation at @p offset.
        /// Returns VK_NULL_HANDLE if not found.  O(log n).
        VmaVirtualAllocation findHandleAt(uint64_t offset) const;

    private:
        struct LiveEntry
        {
            uint64_t offset;
            uint64_t size;
            VmaVirtualAllocation handle;
        };

        static uint64_t nextPow2(uint64_t v);

        VmaVirtualBlock block_{ nullptr };
        uint64_t        total_capacity_{ 0 };
        std::vector<LiveEntry> live_allocs_;  ///< Sorted by offset
    };

} // namespace lux::render

