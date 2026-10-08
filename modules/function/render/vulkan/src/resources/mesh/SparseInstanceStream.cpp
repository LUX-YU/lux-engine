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
        const bool is_missing_device = !device.logicalDevice() || !device.vmaAllocator();
        if (is_missing_device)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
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

    SparseInstanceStreamStorage::~SparseInstanceStreamStorage()
    {
        shutdown();
    }

    bool SparseInstanceStreamStorage::init(
        DeviceContext* device_context,
        std::uint32_t stride,
        std::uint32_t initial_capacity,
        bool sparse_bda
    )
    {
        shutdown();
        device_context_ = device_context;
        stride_ = stride;
        sparse_bda_ = sparse_bda;
        return reserve(initial_capacity);
    }

    void SparseInstanceStreamStorage::shutdown()
    {
        if (device_context_)
        {
            for (auto& page : gpu_pages_)
                destroyBuffer(page.buffer, page.allocation, true);
            destroyBuffer(flat_buffer_, flat_allocation_, true);
        }
        flat_buffer_ = VK_NULL_HANDLE;
        flat_allocation_ = nullptr;
        cpu_pages_.clear();
        gpu_pages_.clear();
        dirty_upload_pages_.clear();
        dirty_upload_flags_.clear();
        capacity_ = 0u;
        stride_ = 0u;
        sparse_bda_ = false;
        device_context_ = nullptr;
    }

    bool SparseInstanceStreamStorage::createGpuPage(GpuPage& page)
    {
        auto candidate = createBuffer(
            *device_context_,
            static_cast<VkDeviceSize>(stride_) * kInstanceSlotsPerPage,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            false
        );
        if (!candidate)
        {
            return false;
        }
        const auto address = bufferAddress(*device_context_, candidate->buffer.buffer());
        if (address == 0)
        {
            return false;
        }
        const auto allocation = candidate->buffer.release();
        page = GpuPage{allocation.buffer, allocation.allocation, address};
        return true;
    }

    bool SparseInstanceStreamStorage::createFlatBuffer(std::uint32_t capacity)
    {
        auto candidate = createBuffer(
            *device_context_,
            static_cast<VkDeviceSize>(stride_) * capacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            false
        );
        if (!candidate)
        {
            return false;
        }
        destroyBuffer(flat_buffer_, flat_allocation_, true);
        const auto allocation = candidate->buffer.release();
        flat_buffer_ = allocation.buffer;
        flat_allocation_ = allocation.allocation;
        return true;
    }

    bool SparseInstanceStreamStorage::reserve(std::uint32_t new_capacity)
    {
        if (new_capacity <= capacity_)
            return true;
        const auto required_pages = (new_capacity - 1u) / kInstanceSlotsPerPage + 1u;
        const auto old_pages = static_cast<std::uint32_t>(cpu_pages_.size());
        cpu_pages_.reserve(required_pages);
        gpu_pages_.reserve(required_pages);
        while (cpu_pages_.size() < required_pages)
        {
            auto cpu = std::make_unique<std::byte[]>(static_cast<std::size_t>(stride_) * kInstanceSlotsPerPage);
            std::memset(cpu.get(), 0, static_cast<std::size_t>(stride_) * kInstanceSlotsPerPage);
            cpu_pages_.push_back(std::move(cpu));
            if (sparse_bda_)
            {
                GpuPage page;
                if (!createGpuPage(page))
                {
                    rollbackPages(old_pages);
                    return false;
                }
                gpu_pages_.push_back(page);
            }
        }

        const auto rounded_capacity = required_pages * kInstanceSlotsPerPage;
        if (!sparse_bda_ && !createFlatBuffer(rounded_capacity))
        {
            rollbackPages(old_pages);
            return false;
        }
        capacity_ = rounded_capacity;
        dirty_upload_flags_.resize(required_pages * (kInstanceSlotsPerPage / kUploadSlotsPerPage), 0u);
        return true;
    }

    void SparseInstanceStreamStorage::rollbackPages(std::uint32_t page_count)
    {
        while (gpu_pages_.size() > page_count)
        {
            auto page = gpu_pages_.back();
            gpu_pages_.pop_back();
            destroyBuffer(page.buffer, page.allocation, false);
        }
        while (cpu_pages_.size() > page_count)
            cpu_pages_.pop_back();
        capacity_ = page_count * kInstanceSlotsPerPage;
        dirty_upload_flags_.resize(page_count * (kInstanceSlotsPerPage / kUploadSlotsPerPage));
        std::erase_if(dirty_upload_pages_, [&](std::uint32_t page) { return page >= dirty_upload_flags_.size(); });
    }

    std::byte* SparseInstanceStreamStorage::at(std::uint32_t index) noexcept
    {
        return cpu_pages_[index >> kInstancePageOffsetBits].get() +
               static_cast<std::size_t>(index & (kInstanceSlotsPerPage - 1u)) * stride_;
    }

    const std::byte* SparseInstanceStreamStorage::at(std::uint32_t index) const noexcept
    {
        return cpu_pages_[index >> kInstancePageOffsetBits].get() +
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
                .destination = sparse_bda_ ? gpu_pages_[physical_page].buffer : flat_buffer_,
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
        return page_index < gpu_pages_.size() ? gpu_pages_[page_index].buffer : VK_NULL_HANDLE;
    }

    VkDeviceAddress SparseInstanceStreamStorage::pageAddress(std::uint32_t page_index) const noexcept
    {
        return page_index < gpu_pages_.size() ? gpu_pages_[page_index].address : 0u;
    }

    void SparseInstanceStreamStorage::destroyBuffer(VkBuffer buffer, VmaAllocation allocation, bool published)
    {
        if (buffer == VK_NULL_HANDLE)
            return;
        if (published && deferred_queue_)
            deferred_queue_->retireBuffer(buffer, allocation);
        else
            vmaDestroyBuffer(device_context_->vmaAllocator(), buffer, allocation);
    }
} // namespace lux::render
