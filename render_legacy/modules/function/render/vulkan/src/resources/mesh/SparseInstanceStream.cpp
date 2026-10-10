#include <lux/engine/render/resources/mesh/SparseInstanceStream.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>

#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace lux::render
{
    namespace
    {
        struct BufferAllocation
        {
            VmaBuffer buffer;
            void* mapped;
        };

        [[nodiscard]] Expected<BufferAllocation>
        createBuffer(DeviceContext& device, VkDeviceSize bytes, VkBufferUsageFlags usage, bool host_visible) noexcept
        {
            VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer_info.size = bytes;
            buffer_info.usage = usage;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo allocation_info{};
            allocation_info.usage =
                host_visible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (host_visible)
            {
                allocation_info.flags =
                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            }
            VkBuffer buffer{};
            VmaAllocation allocation{};
            VmaAllocationInfo mapped{};
            const auto status =
                vmaCreateBuffer(device.vmaAllocator(), &buffer_info, &allocation_info, &buffer, &allocation, &mapped);
            if (status != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
            }
            auto owner = VmaBuffer::adopt({device.vmaAllocator(), buffer, allocation});
            if (host_visible && !mapped.pMappedData)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_MEMORY_MAP_FAILED));
            }
            return BufferAllocation{std::move(owner), mapped.pMappedData};
        }

        [[nodiscard]] VkDeviceAddress bufferAddress(DeviceContext& device, VkBuffer buffer)
        {
            VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            info.buffer = buffer;
            return vkGetBufferDeviceAddress(device.logicalDevice(), &info);
        }
    } // namespace

    SparseInstancePageTable::CreateResult
    SparseInstancePageTable::create(DeviceContext& device, DeferredDestroyQueue& retirement) noexcept
    {
        auto root = createBuffer(
            device,
            rootBufferBytes(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            true
        );
        if (!root)
        {
            return lux::cxx::unexpected(root.error());
        }
        std::memset(root->mapped, 0, static_cast<std::size_t>(rootBufferBytes()));
        const auto status = vmaFlushAllocation(device.vmaAllocator(), root->buffer.allocation(), 0, rootBufferBytes());
        if (status != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
        }
        return std::unique_ptr<SparseInstancePageTable>(new SparseInstancePageTable(
            device,
            retirement,
            std::move(root->buffer),
            static_cast<VkDeviceAddress*>(root->mapped)
        ));
    }

    SparseInstancePageTable::SparseInstancePageTable(
        DeviceContext& device,
        DeferredDestroyQueue& retirement,
        VmaBuffer buffer,
        VkDeviceAddress* mapped
    ) noexcept
        : device_(device), root_mapped_(mapped), leaves_(kInstancePageTableAxisSize)
    {
        const auto allocation = buffer.release();
        root_buffer_ = TFifOwnedAllocated<VkBuffer>{retirement, allocation.buffer, allocation.allocation};
    }

    Expected<void>
    SparseInstancePageTable::publish(std::uint32_t page_index, const GpuInstancePageAddresses& addresses) noexcept
    {
        const auto root_index = page_index >> kInstancePageTableAxisBits;
        const auto leaf_index = page_index & (kInstancePageTableAxisSize - 1u);
        if (root_index >= leaves_.size())
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        auto& leaf = leaves_[root_index];
        if (leaf.buffer.valid())
        {
            const auto previous = leaf.mapped[leaf_index];
            leaf.mapped[leaf_index] = addresses;
            const auto status = vmaFlushAllocation(
                device_.vmaAllocator(),
                leaf.buffer.alloc(),
                sizeof(GpuInstancePageAddresses) * leaf_index,
                sizeof(GpuInstancePageAddresses)
            );
            if (status != VK_SUCCESS)
            {
                leaf.mapped[leaf_index] = previous;
                // Restore the accepted CPU bytes; this render-thread operation admits no GPU work.
                (void)vmaFlushAllocation(
                    device_.vmaAllocator(),
                    leaf.buffer.alloc(),
                    sizeof(GpuInstancePageAddresses) * leaf_index,
                    sizeof(GpuInstancePageAddresses)
                );
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
            }
            return {};
        }
        constexpr auto leaf_bytes = sizeof(GpuInstancePageAddresses) * kInstancePageTableAxisSize;
        auto candidate = createBuffer(
            device_,
            leaf_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            true
        );
        if (!candidate)
        {
            return lux::cxx::unexpected(candidate.error());
        }
        auto* mapped = static_cast<GpuInstancePageAddresses*>(candidate->mapped);
        std::memset(mapped, 0, leaf_bytes);
        mapped[leaf_index] = addresses;
        const auto leaf_status =
            vmaFlushAllocation(device_.vmaAllocator(), candidate->buffer.allocation(), 0, leaf_bytes);
        if (leaf_status != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(leaf_status));
        }
        const auto address = bufferAddress(device_, candidate->buffer.buffer());
        if (address == 0)
        {
            return renderFailure<err::memory::BufferDeviceAddressUnavailable>();
        }
        root_mapped_[root_index] = address;
        const auto root_status = vmaFlushAllocation(
            device_.vmaAllocator(),
            root_buffer_.alloc(),
            sizeof(VkDeviceAddress) * root_index,
            sizeof(VkDeviceAddress)
        );
        if (root_status != VK_SUCCESS)
        {
            root_mapped_[root_index] = 0;
            (void)vmaFlushAllocation(
                device_.vmaAllocator(),
                root_buffer_.alloc(),
                sizeof(VkDeviceAddress) * root_index,
                sizeof(VkDeviceAddress)
            );
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(root_status));
        }
        const auto allocation = candidate->buffer.release();
        leaf.buffer = TFifOwnedAllocated<VkBuffer>{root_buffer_.queue(), allocation.buffer, allocation.allocation};
        leaf.mapped = mapped;
        return {};
    }

    std::uint32_t SparseInstancePageTable::leafCount() const noexcept
    {
        return static_cast<std::uint32_t>(
            std::count_if(leaves_.begin(), leaves_.end(), [](const Leaf& leaf) { return leaf.buffer.valid(); })
        );
    }

    Expected<std::vector<SparseInstanceStreamStorage::Page>>
    SparseInstanceStreamStorage::preparePages(
        DeviceContext& device,
        std::uint32_t stride,
        std::uint32_t count,
        bool sparse_bda
    ) noexcept
    {
        std::vector<Page> pages;
        pages.reserve(count);
        const auto bytes = static_cast<VkDeviceSize>(stride) * kInstanceSlotsPerPage;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            auto cpu = std::make_unique<std::byte[]>(static_cast<std::size_t>(bytes));
            Page page{std::move(cpu), {}, 0};
            if (sparse_bda)
            {
                auto candidate = createBuffer(
                    device,
                    bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    false
                );
                if (!candidate)
                {
                    return lux::cxx::unexpected(candidate.error());
                }
                page.address = bufferAddress(device, candidate->buffer.buffer());
                if (page.address == 0)
                {
                    return renderFailure<err::memory::BufferDeviceAddressUnavailable>();
                }
                page.gpu = std::move(candidate->buffer);
            }
            pages.push_back(std::move(page));
        }
        return pages;
    }

    Expected<SparseInstanceStreamStorage>
    SparseInstanceStreamStorage::create(
        DeviceContext& device,
        DeferredDestroyQueue& retirement,
        std::uint32_t stride,
        std::uint32_t initial_capacity,
        bool sparse_bda
    ) noexcept
    {
        constexpr auto max_capacity = UINT32_MAX - (kInstanceSlotsPerPage - 1u);
        const bool is_invalid_extent = stride == 0 || initial_capacity == 0 || initial_capacity > max_capacity;
        if (is_invalid_extent)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        const auto count = (initial_capacity - 1u) / kInstanceSlotsPerPage + 1u;
        auto pages = preparePages(device, stride, count, sparse_bda);
        if (!pages)
        {
            return lux::cxx::unexpected(pages.error());
        }
        VmaBuffer flat;
        if (!sparse_bda)
        {
            auto candidate = createBuffer(
                device,
                static_cast<VkDeviceSize>(count) * kInstanceSlotsPerPage * stride,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                false
            );
            if (!candidate)
            {
                return lux::cxx::unexpected(candidate.error());
            }
            flat = std::move(candidate->buffer);
        }
        return SparseInstanceStreamStorage{device, retirement, stride, sparse_bda, std::move(*pages), std::move(flat)};
    }

    SparseInstanceStreamStorage::SparseInstanceStreamStorage(
        DeviceContext& device,
        DeferredDestroyQueue& retirement,
        std::uint32_t stride,
        bool sparse_bda,
        std::vector<Page> pages,
        VmaBuffer flat_buffer
    ) noexcept
        : device_(device), retirement_(retirement), stride_(stride), sparse_bda_(sparse_bda), pages_(std::move(pages)),
          dirty_upload_flags_(pages_.size() * (kInstanceSlotsPerPage / kUploadSlotsPerPage))
    {
        if (flat_buffer)
        {
            const auto allocation = flat_buffer.release();
            flat_buffer_ = TFifOwnedAllocated<VkBuffer>{retirement_, allocation.buffer, allocation.allocation};
        }
    }

    SparseInstanceStreamStorage::~SparseInstanceStreamStorage() noexcept
    {
        // Pages accepted by this stream may be in flight. Unpublished candidates and rollback tails
        // remain local VmaBuffer owners; only the surviving stream pages transfer to serial retirement.
        for (auto& page : pages_)
        {
            if (page.gpu)
            {
                const auto allocation = page.gpu.release();
                retirement_.retireBuffer(allocation.buffer, allocation.allocation);
            }
        }
    }

    Expected<void> SparseInstanceStreamStorage::reserve(std::uint32_t capacity) noexcept
    {
        auto candidate = prepareGrowth(capacity);
        if (!candidate)
        {
            return lux::cxx::unexpected(candidate.error());
        }
        commitGrowth(std::move(*candidate));
        return {};
    }

    Expected<SparseInstanceStreamStorage::Growth>
    SparseInstanceStreamStorage::prepareGrowth(std::uint32_t new_capacity) noexcept
    {
        if (new_capacity <= capacity())
        {
            return Growth{};
        }
        constexpr auto max_capacity = UINT32_MAX - (kInstanceSlotsPerPage - 1u);
        if (new_capacity > max_capacity)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        const auto required_pages = (new_capacity - 1u) / kInstanceSlotsPerPage + 1u;
        auto candidates = preparePages(device_, stride_, required_pages - pageCount(), sparse_bda_);
        if (!candidates)
        {
            return lux::cxx::unexpected(candidates.error());
        }
        VmaBuffer flat;
        if (!sparse_bda_)
        {
            auto candidate = createBuffer(
                device_,
                static_cast<VkDeviceSize>(required_pages) * kInstanceSlotsPerPage * stride_,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                false
            );
            if (!candidate)
            {
                return lux::cxx::unexpected(candidate.error());
            }
            flat = std::move(candidate->buffer);
        }
        // Prepare storage without changing extent, accepted descriptors or dirty bytes.
        pages_.reserve(required_pages);
        dirty_upload_flags_.reserve(required_pages * (kInstanceSlotsPerPage / kUploadSlotsPerPage));
        return Growth{std::move(*candidates), std::move(flat)};
    }

    void SparseInstanceStreamStorage::commitGrowth(Growth growth) noexcept
    {
        for (auto& page : growth.pages)
        {
            pages_.push_back(std::move(page));
        }
        dirty_upload_flags_.resize(pages_.size() * (kInstanceSlotsPerPage / kUploadSlotsPerPage), 0);
        if (growth.flat)
        {
            const auto allocation = growth.flat.release();
            flat_buffer_ = TFifOwnedAllocated<VkBuffer>{retirement_, allocation.buffer, allocation.allocation};
        }
    }

    VkDeviceAddress SparseInstanceStreamStorage::pageAddress(const Growth& growth, std::uint32_t index) const noexcept
    {
        return index < pages_.size() ? pages_[index].address : growth.pages[index - pages_.size()].address;
    }

    std::byte* SparseInstanceStreamStorage::at(std::uint32_t index) noexcept
    {
        return pages_[index >> kInstancePageOffsetBits].cpu.get() +
               static_cast<std::size_t>(index & (kInstanceSlotsPerPage - 1u)) * stride_;
    }

    const std::byte* SparseInstanceStreamStorage::at(std::uint32_t index) const noexcept
    {
        return pages_[index >> kInstancePageOffsetBits].cpu.get() +
               static_cast<std::size_t>(index & (kInstanceSlotsPerPage - 1u)) * stride_;
    }

    void SparseInstanceStreamStorage::markDirty(std::uint32_t index)
    {
        const auto page = index / kUploadSlotsPerPage;
        if (dirty_upload_flags_[page] == 0u)
        {
            dirty_upload_flags_[page] = 1u;
            dirty_upload_pages_.push_back(page);
        }
    }

    VkDeviceSize SparseInstanceStreamStorage::collectUploadChunks(
        std::uint32_t count,
        bool full_upload,
        std::vector<UploadChunk>& chunks
    )
    {
        VkDeviceSize total = 0u;
        const auto append = [&](std::uint32_t first, std::uint32_t slots) {
            if (slots == 0u)
                return;
            const auto physical_page = first >> kInstancePageOffsetBits;
            const auto page_offset = first & (kInstanceSlotsPerPage - 1u);
            const auto size = static_cast<VkDeviceSize>(slots) * stride_;
            chunks.push_back(UploadChunk{
                .src = at(first),
                .destination = sparse_bda_ ? pages_[physical_page].gpu.buffer() : flat_buffer_.get(),
                .destination_offset = sparse_bda_ ? static_cast<VkDeviceSize>(page_offset) * stride_
                                                  : static_cast<VkDeviceSize>(first) * stride_,
                .size = size,
            });
            total += size;
        };

        if (full_upload)
        {
            std::uint32_t first = 0u;
            while (first < count)
            {
                const auto slots = std::min(kInstanceSlotsPerPage, count - first);
                append(first, slots);
                first += slots;
            }
            return total;
        }

        std::ranges::sort(dirty_upload_pages_);
        for (const auto upload_page : dirty_upload_pages_)
        {
            const auto first = upload_page * kUploadSlotsPerPage;
            if (first >= count)
                continue;
            append(first, std::min(kUploadSlotsPerPage, count - first));
        }
        return total;
    }

    void SparseInstanceStreamStorage::clearDirtyState()
    {
        for (const auto page : dirty_upload_pages_)
            if (page < dirty_upload_flags_.size())
                dirty_upload_flags_[page] = 0u;
        dirty_upload_pages_.clear();
    }

    VkBuffer SparseInstanceStreamStorage::pageBuffer(std::uint32_t page_index) const noexcept
    {
        return page_index < pages_.size() ? pages_[page_index].gpu.buffer() : VK_NULL_HANDLE;
    }

    VkDeviceAddress SparseInstanceStreamStorage::pageAddress(std::uint32_t page_index) const noexcept
    {
        return page_index < pages_.size() ? pages_[page_index].address : 0u;
    }

} // namespace lux::render
