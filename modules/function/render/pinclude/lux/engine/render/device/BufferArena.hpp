#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <vk_mem_alloc.h>

namespace lux::render::detail
{
    struct ArenaAllocationId final
    {
        std::uint64_t arena = 0;
        cxx::SlotKey<struct ArenaAllocationTag> slot;
    };

    struct BufferSlice final
    {
        nri::Buffer* buffer = nullptr;
        std::uint64_t offset = 0, size = 0;
    };

    struct ArenaAllocation final
    {
        ArenaAllocationId id;
        BufferSlice slice;
    };

    class BufferArena final
    {
    public:
        BufferArena(
            DeviceSession& session,
            nri::BufferUsageBits usage,
            std::uint64_t segment_bytes = 4 * 1024 * 1024
        ) noexcept;

        ~BufferArena() noexcept;
        BufferArena(const BufferArena&) = delete;
        BufferArena& operator=(const BufferArena&) = delete;
        [[nodiscard]] RenderResult<ArenaAllocation> allocate(std::uint64_t bytes, std::uint64_t alignment) noexcept;
        [[nodiscard]] bool retireAllocation(ArenaAllocationId, SubmissionFrontiers) noexcept;

        [[nodiscard]] std::uint64_t capacity() const noexcept
        {
            return capacity_;
        }

        [[nodiscard]] std::size_t segmentCount() const noexcept
        {
            return storage_->segments.size();
        }

    private:
        struct Segment final
        {
            nri::Buffer* buffer = nullptr;
            VmaVirtualBlock block = VK_NULL_HANDLE;
        };

        struct Allocation final
        {
            Segment* segment;
            VmaVirtualAllocation allocation;
            bool retiring = false;
        };

        struct Storage final
        {
            const nri::CoreInterface& core;
            std::vector<std::unique_ptr<Segment>> segments;
            cxx::StableSlotMap<Allocation, ArenaAllocationTag> allocations;
            std::size_t pending = 0;
            bool closed = false;

            ~Storage() noexcept;
        };

        static void destroySegment(const nri::CoreInterface&, Segment&) noexcept;
        [[nodiscard]] ArenaAllocation allocate(Segment&, std::uint64_t bytes, std::uint64_t alignment) noexcept;
        DeviceSession& session_;
        nri::BufferUsageBits usage_;
        std::uint64_t capacity_ = 0;
        std::uint64_t segment_bytes_;
        std::uint64_t identity_;
        std::unique_ptr<Storage> storage_;
    };
} // namespace lux::render::detail
