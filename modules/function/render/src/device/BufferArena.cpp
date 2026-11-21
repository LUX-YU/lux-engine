#include <lux/engine/render/device/BufferArena.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <utility>

namespace lux::render::detail
{
    namespace
    {
        std::atomic<std::uint64_t> next_arena_identity{1};
    }

    BufferArena::BufferArena(DeviceSession& session, nri::BufferUsageBits usage, std::uint64_t segment_bytes) noexcept
        : session_(session), usage_(usage), segment_bytes_(segment_bytes),
          identity_(next_arena_identity.fetch_add(1, std::memory_order_relaxed)),
          storage_(new Storage{session.nriCore()})
    {
        if (!identity_) std::terminate();
    }

    bool BufferArena::retireAllocation(ArenaAllocationId id, SubmissionFrontiers uses) noexcept
    {
        if (id.arena != identity_) return false;
        auto* storage = storage_.get();
        auto* row = storage->allocations.find(id.slot);
        if (!row || row->retiring) return false;
        row->retiring = true;
        ++storage->pending;
        session_.deferRelease(std::move(uses), [storage, slot = id.slot]() noexcept
        {
            auto* allocation = storage->allocations.find(slot);
            if (!allocation || !allocation->retiring) std::terminate();
            vmaVirtualFree(allocation->segment->block, allocation->allocation);
            storage->allocations.erase(slot);
            if (--storage->pending == 0 && storage->closed)
            {
                delete storage;
            }
        });
        return true;
    }

    void BufferArena::destroySegment(const nri::CoreInterface& nri_core, Segment& segment) noexcept
    {
        if (segment.block)
        {
            vmaDestroyVirtualBlock(std::exchange(segment.block, VK_NULL_HANDLE));
        }
        nri_core.DestroyBuffer(std::exchange(segment.buffer, nullptr));
    }

    BufferArena::Storage::~Storage() noexcept
    {
        if (!allocations.empty() || pending) std::terminate();
        for (auto& segment : segments) destroySegment(core, *segment);
    }

    BufferArena::~BufferArena() noexcept
    {
        auto& storage = *storage_;
        if (storage.allocations.size() != storage.pending) std::terminate();
        if (storage.pending)
        {
            // The last accepted retirement returns its range before destroying the backing segments.
            storage.closed = true;
            (void)storage_.release();
        }
    }

    ArenaAllocation BufferArena::allocate(Segment& segment, std::uint64_t bytes, std::uint64_t alignment) noexcept
    {
        VmaVirtualAllocationCreateInfo info{};
        info.size = bytes;
        info.alignment = alignment;
        VmaVirtualAllocation allocation = VK_NULL_HANDLE;
        std::uint64_t offset = 0;
        if (vmaVirtualAllocate(segment.block, &info, &allocation, &offset) != VK_SUCCESS)
        {
            return {};
        }
        const auto slot = storage_->allocations.emplace(Allocation{&segment, allocation});
        return {{identity_, slot}, {segment.buffer, offset, bytes}};
    }

    RenderResult<ArenaAllocation> BufferArena::allocate(std::uint64_t bytes, std::uint64_t alignment) noexcept
    {
        if (!bytes)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        if (!storage_->segments.empty())
        {
            auto result = allocate(*storage_->segments.back(), bytes, alignment);
            if (result.slice.buffer)
            {
                return result;
            }
            for (std::size_t i = 0; i + 1 < storage_->segments.size(); ++i)
            {
                result = allocate(*storage_->segments[i], bytes, alignment);
                if (result.slice.buffer)
                {
                    return result;
                }
            }
        }
        const auto size = std::max(segment_bytes_, bytes);
        auto candidate = std::make_unique<Segment>();
        VmaVirtualBlockCreateInfo info{};
        info.size = size;
        const auto status = vmaCreateVirtualBlock(&info, &candidate->block);
        if (status != VK_SUCCESS)
        {
            if (status == VK_ERROR_OUT_OF_HOST_MEMORY)
            {
                std::terminate();
            }
            // Virtual metadata uses the existing VMA algorithm, not a second physical allocator.
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        cxx::scope_exit rollback([&]() noexcept { destroySegment(session_.nriCore(), *candidate); });
        const nri::BufferDesc description{size, 4, usage_};
        const auto created =
            session_.nriCore().CreatePlacedBuffer(session_.device(), NriDeviceHeap, description, candidate->buffer);
        if (created != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(created));
        }
        auto result = allocate(*candidate, bytes, alignment);
        if (!result.slice.buffer)
        {
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        rollback.release();
        capacity_ += size;
        storage_->segments.push_back(std::move(candidate));
        return result;
    }
} // namespace lux::render::detail
