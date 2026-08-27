#include <lux/engine/render/resources/mesh/SparseInstanceStream.hpp>

#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace lux::render
{
    namespace
    {
        [[nodiscard]] bool createBuffer(
            DeviceContext& device,
            VkDeviceSize bytes,
            VkBufferUsageFlags usage,
            bool host_visible,
            VkBuffer& buffer,
            VmaAllocation& allocation,
            void** mapped
        )
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
            VmaAllocationInfo result_info{};
            const auto result = vmaCreateBuffer(
                device.vmaAllocator(),
                &buffer_info,
                &allocation_info,
                &buffer,
                &allocation,
                &result_info
            );
            if (result != VK_SUCCESS)
                return false;
            if (mapped)
                *mapped = result_info.pMappedData;
            return true;
        }

        [[nodiscard]] VkDeviceAddress bufferAddress(DeviceContext& device, VkBuffer buffer)
        {
            VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            info.buffer = buffer;
            return vkGetBufferDeviceAddress(device.logicalDevice(), &info);
        }
    } // namespace

    SparseInstancePageTable::~SparseInstancePageTable()
    {
        shutdown();
    }

    bool SparseInstancePageTable::init(DeviceContext* device_context)
    {
        shutdown();
        device_context_ = device_context;
        void* mapped = nullptr;
        if (!device_context_ || !createBuffer(
                                    *device_context_,
                                    rootBufferBytes(),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    true,
                                    root_buffer_,
                                    root_allocation_,
                                    &mapped))
        {
            shutdown();
            return false;
        }
        root_mapped_ = static_cast<VkDeviceAddress*>(mapped);
        std::memset(root_mapped_, 0, static_cast<std::size_t>(rootBufferBytes()));
        vmaFlushAllocation(device_context_->vmaAllocator(), root_allocation_, 0u, rootBufferBytes());
        leaves_.resize(kInstancePageTableAxisSize);
        return true;
    }

    void SparseInstancePageTable::shutdown()
    {
        if (device_context_)
        {
            for (auto& leaf : leaves_)
                destroyBuffer(leaf.buffer, leaf.allocation);
            destroyBuffer(root_buffer_, root_allocation_);
        }
        leaves_.clear();
        root_buffer_ = VK_NULL_HANDLE;
        root_allocation_ = nullptr;
        root_mapped_ = nullptr;
        device_context_ = nullptr;
    }

    bool SparseInstancePageTable::createLeaf(std::uint32_t root_index)
    {
        if (root_index >= leaves_.size())
            return false;
        auto& leaf = leaves_[root_index];
        if (leaf.buffer != VK_NULL_HANDLE)
            return true;
        void* mapped = nullptr;
        if (!createBuffer(
                *device_context_,
                sizeof(GpuInstancePageAddresses) * kInstancePageTableAxisSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                true,
                leaf.buffer,
                leaf.allocation,
                &mapped))
        {
            return false;
        }
        leaf.mapped = static_cast<GpuInstancePageAddresses*>(mapped);
        std::memset(leaf.mapped, 0, sizeof(GpuInstancePageAddresses) * kInstancePageTableAxisSize);
        vmaFlushAllocation(device_context_->vmaAllocator(), leaf.allocation, 0u, VK_WHOLE_SIZE);
        leaf.address = bufferAddress(*device_context_, leaf.buffer);
        if (leaf.address == 0u)
        {
            vmaDestroyBuffer(device_context_->vmaAllocator(), leaf.buffer, leaf.allocation);
            leaf = {};
            return false;
        }
        root_mapped_[root_index] = leaf.address;
        vmaFlushAllocation(
            device_context_->vmaAllocator(),
            root_allocation_,
            sizeof(VkDeviceAddress) * root_index,
            sizeof(VkDeviceAddress)
        );
        return true;
    }

    bool SparseInstancePageTable::publish(std::uint32_t page_index, const GpuInstancePageAddresses& addresses)
    {
        const auto root_index = page_index >> kInstancePageTableAxisBits;
        const auto leaf_index = page_index & (kInstancePageTableAxisSize - 1u);
        if (!createLeaf(root_index))
            return false;
        auto& leaf = leaves_[root_index];
        leaf.mapped[leaf_index] = addresses;
        vmaFlushAllocation(
            device_context_->vmaAllocator(),
            leaf.allocation,
            sizeof(GpuInstancePageAddresses) * leaf_index,
            sizeof(GpuInstancePageAddresses)
        );
        return true;
    }

    std::uint32_t SparseInstancePageTable::leafCount() const noexcept
    {
        return static_cast<std::uint32_t>(std::count_if(leaves_.begin(), leaves_.end(), [](const Leaf& leaf) {
            return leaf.buffer != VK_NULL_HANDLE;
        })
        );
    }

    void SparseInstancePageTable::destroyBuffer(VkBuffer buffer, VmaAllocation allocation)
    {
        if (buffer == VK_NULL_HANDLE)
            return;
        if (deferred_queue_)
            deferred_queue_->retireBuffer(buffer, allocation);
        else
            vmaDestroyBuffer(device_context_->vmaAllocator(), buffer, allocation);
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
        if (!createBuffer(
                *device_context_,
                static_cast<VkDeviceSize>(stride_) * kInstanceSlotsPerPage,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                false,
                page.buffer,
                page.allocation,
                nullptr))
        {
            return false;
        }
        page.address = bufferAddress(*device_context_, page.buffer);
        if (page.address != 0u)
            return true;
        vmaDestroyBuffer(device_context_->vmaAllocator(), page.buffer, page.allocation);
        page = {};
        return false;
    }

    bool SparseInstanceStreamStorage::createFlatBuffer(std::uint32_t capacity)
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        if (!createBuffer(
                *device_context_,
                static_cast<VkDeviceSize>(stride_) * capacity,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                false,
                buffer,
                allocation,
                nullptr))
        {
            return false;
        }
        destroyBuffer(flat_buffer_, flat_allocation_, true);
        flat_buffer_ = buffer;
        flat_allocation_ = allocation;
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
            }
            );
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
