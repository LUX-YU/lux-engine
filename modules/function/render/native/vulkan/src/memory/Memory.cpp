#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

#include "Native.hpp"
#include <algorithm>
#include <cstring>
#include <utility>
#include <vk_mem_alloc.h>

namespace lux::render::vulkan
{
    Buffer::~Buffer() noexcept
    {
        LUX_DESTROY("buffer", vmaDestroyBuffer(allocator_->native(), buffer_, allocation_));
    }

    ImageMemory::~ImageMemory() noexcept
    {
        LUX_DESTROY("image_memory", vmaFreeMemory(allocator_->native(), allocation_));
    }

    Image::~Image() noexcept
    {
        LUX_DESTROY("image", vkDestroyImage(memory_->allocator_->device(), image_, nullptr));
        // memory_ releases after VkImage; the last alias releases the allocation.
    }

    RenderResult<std::shared_ptr<Buffer>> VulkanAllocator::createBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        EMemoryAccess access,
        std::span<const std::uint32_t> families
    ) noexcept
    {
        const bool is_invalid_access =
            access != EMemoryAccess::DEVICE && access != EMemoryAccess::UPLOAD && access != EMemoryAccess::READBACK;
        const bool is_invalid_config = size == 0 || usage == 0 || is_invalid_access;
        if (is_invalid_config)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const bool is_unsupported_address = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) && !buffer_device_address_;
        if (is_unsupported_address)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = families.empty() ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = static_cast<std::uint32_t>(families.size());
        info.pQueueFamilyIndices = families.data();
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO;
        if (access == EMemoryAccess::DEVICE)
        {
            memory.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
        else
        {
            memory.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                           (access == EMemoryAccess::UPLOAD ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                                            : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
            memory.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        }
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VmaAllocationInfo mapped{};
        const auto result =
            LUX_NATIVE("buffer", vmaCreateBuffer(native(), &info, &memory, &buffer, &allocation, &mapped));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        if (access != EMemoryAccess::DEVICE)
        {
            const auto mapping = LUX_NATIVE("mapping", mapped.pMappedData ? VK_SUCCESS : VK_ERROR_MEMORY_MAP_FAILED);
            if (mapping != VK_SUCCESS)
            {
                LUX_DESTROY("buffer", vmaDestroyBuffer(native(), buffer, allocation));
                return cxx::unexpected(nativeError(mapping));
            }
        }
        return std::make_shared<Buffer>(*this, buffer, allocation, mapped.pMappedData, size, usage, access);
    }

    RenderResult<void> Buffer::write(VkDeviceSize offset,
        std::span<const std::byte> bytes) noexcept
    {
        const bool is_invalid_range = offset > size_ || bytes.size() > size_ - offset;
        const bool is_invalid_access = access_ == EMemoryAccess::DEVICE || is_invalid_range;
        if (is_invalid_access)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (bytes.empty())
        {
            return {};
        }
        std::memcpy(static_cast<std::byte*>(mapped_) + offset, bytes.data(), bytes.size());
        const auto result = LUX_NATIVE("flush",
            vmaFlushAllocation(allocator_->native(), allocation_, offset, bytes.size())
        );
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        return {};
    }

    RenderResult<void> Buffer::flush(VkDeviceSize offset, VkDeviceSize size) noexcept
    {
        const bool is_invalid_range = offset > size_ || size > size_ - offset;
        const bool is_invalid_access = !mapped_ || is_invalid_range;
        if (is_invalid_access) return cxx::unexpected(RenderError{kInvalidArgument});
        if (!size) return {};
        const auto result = LUX_NATIVE("flush", vmaFlushAllocation(allocator_->native(), allocation_, offset, size));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        return {};
    }

    RenderResult<void> Buffer::read(VkDeviceSize offset,
        std::span<std::byte> bytes) const noexcept
    {
        const bool is_invalid_range = offset > size_ || bytes.size() > size_ - offset;
        const bool is_invalid_access = access_ != EMemoryAccess::READBACK || is_invalid_range;
        if (is_invalid_access)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (bytes.empty())
        {
            return {};
        }
        const auto result =
            LUX_NATIVE("invalidate",
                vmaInvalidateAllocation(allocator_->native(), allocation_, offset, bytes.size())
            );
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        std::memcpy(bytes.data(), static_cast<const std::byte*>(mapped_) + offset, bytes.size());
        return {};
    }

    RenderResult<ImagePermit> DeviceSession::createImage(
        VkExtent2D extent,
        VkFormat format,
        VkImageUsageFlags usage,
        VkSampleCountFlagBits samples
    ) noexcept
    {
        return createImage(ImageDescription{extent, format, usage, samples});
    }

    RenderResult<std::shared_ptr<Image>> VulkanAllocator::createImage(const ImageDescription& description) noexcept
    {
        const auto extent = description.extent;
        const auto format = description.format;
        const auto usage = description.usage;
        const auto samples = description.samples;
        const auto mip_levels = description.mip_levels;
        const auto array_layers = description.array_layers;
        std::uint32_t max_levels = 0;
        for (auto dimension = std::max(extent.width, extent.height); dimension != 0; dimension >>= 1)
        {
            ++max_levels;
        }
        const bool invalid_subresources = mip_levels == 0 || array_layers == 0 || mip_levels > max_levels ||
                                          (samples != VK_SAMPLE_COUNT_1_BIT && mip_levels != 1);
        const bool is_invalid_config =
            extent.width == 0 || extent.height == 0 || format == VK_FORMAT_UNDEFINED || usage == 0 || samples == 0 ||
            (static_cast<std::uint32_t>(samples) & (static_cast<std::uint32_t>(samples) - 1)) != 0;
        if (is_invalid_config || invalid_subresources)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VmaAllocatorInfo allocator_info{};
        vmaGetAllocatorInfo(native(), &allocator_info);
        VkImageFormatProperties supported{};
        const auto support = vkGetPhysicalDeviceImageFormatProperties(
            allocator_info.physicalDevice,
            format,
            VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,
            usage,
            description.aliasable ? VK_IMAGE_CREATE_ALIAS_BIT : 0,
            &supported
        );
        if (support != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(support));
        }
        const bool is_too_large = extent.width > supported.maxExtent.width ||
                                  extent.height > supported.maxExtent.height || mip_levels > supported.maxMipLevels ||
                                  array_layers > supported.maxArrayLayers;
        if (is_too_large || (supported.sampleCounts & samples) == 0)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags = description.aliasable ? VK_IMAGE_CREATE_ALIAS_BIT : 0;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = mip_levels;
        info.arrayLayers = array_layers;
        info.samples = samples;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo memory{};
        memory.flags = description.aliasable ? VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT : 0;
        memory.usage = VMA_MEMORY_USAGE_AUTO;
        memory.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VkImage image{};
        VmaAllocation allocation{};
        const auto result =
            LUX_NATIVE("image", vmaCreateImage(native(), &info, &memory, &image, &allocation, nullptr));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        LUX_RECORD("image_memory", (void)0);
        auto backing = std::make_shared<ImageMemory>(*this, allocation);
        return std::make_shared<Image>(image, std::move(backing), description);
    }

    RenderResult<std::shared_ptr<Image>> VulkanAllocator::aliasImage(const Image& source) noexcept
    {
        if (source.memory_->allocator_ != this)
        {
            return cxx::unexpected(RenderError{kWrongOwner});
        }
        const bool is_invalid_source = !source.description_.aliasable;
        if (is_invalid_source)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto& description = source.description_;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags = VK_IMAGE_CREATE_ALIAS_BIT;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = description.format;
        info.extent = {description.extent.width, description.extent.height, 1};
        info.mipLevels = description.mip_levels;
        info.arrayLayers = description.array_layers;
        info.samples = description.samples;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = description.usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkImage image{};
        auto created = LUX_NATIVE("image_alias", vkCreateImage(device(), &info, nullptr, &image));
        if (created != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(created));
        }

        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        requirements.pNext = &dedicated;
        VkImageMemoryRequirementsInfo2 query{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        query.image = image;
        vkGetImageMemoryRequirements2(device(), &query, &requirements);
        const auto binding = source.memoryBinding();
        const auto& required = requirements.memoryRequirements;
        const bool incompatible = dedicated.requiresDedicatedAllocation || required.size > binding.size ||
                                  (required.memoryTypeBits & (1u << binding.memory_type)) == 0 ||
                                  binding.offset % required.alignment != 0;
        if (incompatible)
        {
            LUX_DESTROY("image_alias", vkDestroyImage(device(), image, nullptr));
            return cxx::unexpected(RenderError{kUnsupported});
        }
        created = LUX_NATIVE("image_alias_bind", vmaBindImageMemory(native(), source.memory_->allocation_, image));
        if (created != VK_SUCCESS)
        {
            LUX_DESTROY("image_alias", vkDestroyImage(device(), image, nullptr));
            return cxx::unexpected(nativeError(created));
        }
        LUX_RECORD("image", (void)0);
        return std::make_shared<Image>(image, source.memory_, description);
    }

    RenderResult<Buffer*> DeviceSession::createBufferRecord(
        VkDeviceSize size, VkBufferUsageFlags usage, EMemoryAccess access,
        std::span<const std::uint32_t> families
    ) noexcept
    {
        auto buffer = allocator_.createBuffer(size, usage, access, families);
        if (!buffer) return cxx::unexpected(buffer.error());
        const auto key = buffers_.emplace(std::move(*buffer));
        auto* record = buffers_.find(key)->get();
        record->key_ = key;
        return record;
    }

    RenderResult<Image*> DeviceSession::createImageRecord(const ImageDescription& description) noexcept
    {
        auto image = allocator_.createImage(description);
        if (!image) return cxx::unexpected(image.error());
        const auto key = images_.emplace(std::move(*image));
        auto* record = images_.find(key)->get();
        record->key_ = key;
        return record;
    }

    RenderResult<ImagePermit> DeviceSession::aliasImage(const ImagePermit& permit) noexcept
    {
        auto* source = borrow(permit);
        if (!source) return cxx::unexpected(RenderError{kWrongOwner});
        auto image = allocator_.aliasImage(*source);
        if (!image) return cxx::unexpected(image.error());
        const auto key = images_.emplace(std::move(*image));
        images_.find(key)->get()->key_ = key;
        return ImagePermit{*this, key};
    }

    RenderResult<BufferPermit> DeviceSession::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, EMemoryAccess access) noexcept
    {
        auto record = createBufferRecord(size, usage, access);
        if (!record) return cxx::unexpected(record.error());
        return BufferPermit{*this, (*record)->key_};
    }

    Buffer* DeviceSession::borrow(const BufferPermit& permit) const noexcept
    {
        if (permit.owner_ != this) return nullptr;
        auto* value = const_cast<DeviceSession*>(this)->buffers_.find(permit.key_);
        return value && !(*value)->retired_ ? value->get() : nullptr;
    }

    RenderResult<void> DeviceSession::destroyUnsubmitted(BufferPermit& permit) noexcept
    {
        auto* value = borrow(permit);
        if (!value) return cxx::unexpected(RenderError{kWrongOwner});
        permit.owner_ = nullptr;
        permit.key_ = {};
        destroyFinished(value);
        return {};
    }

    RenderResult<ImagePermit> DeviceSession::createImage(const ImageDescription& description) noexcept
    {
        auto record = createImageRecord(description);
        if (!record) return cxx::unexpected(record.error());
        return ImagePermit{*this, (*record)->key_};
    }

    Image* DeviceSession::borrow(const ImagePermit& permit) const noexcept
    {
        if (permit.owner_ != this) return nullptr;
        auto* value = const_cast<DeviceSession*>(this)->images_.find(permit.key_);
        return value && !(*value)->retired_ ? value->get() : nullptr;
    }

    RenderResult<void> DeviceSession::destroyUnsubmitted(ImagePermit& permit) noexcept
    {
        auto* value = borrow(permit);
        if (!value) return cxx::unexpected(RenderError{kWrongOwner});
        permit.owner_ = nullptr;
        permit.key_ = {};
        destroyFinished(value);
        return {};
    }

    RenderResult<void> DeviceSession::releaseCompleted(BufferPermit& permit, SubmissionTicket ticket) noexcept
    {
        SubmissionRequirements uses;
        uses.merge(ticket);
        return releaseCompleted(permit, uses);
    }

    RenderResult<void> DeviceSession::releaseCompleted(ImagePermit& permit, SubmissionTicket ticket) noexcept
    {
        SubmissionRequirements uses;
        uses.merge(ticket);
        return releaseCompleted(permit, uses);
    }

    bool DeviceSession::owns(const SubmissionRequirements& uses) const noexcept
    {
        if (uses.empty()) return false;
        for (const auto& use : uses.uses())
        {
            if (!use.queue) continue;
            const auto owner = std::find_if(queue_owners_.begin(), queue_owners_.end(), [&](const auto& queue) {
                return queue.get() == use.queue;
            });
            if (owner == queue_owners_.end() || use.serial == 0 || use.serial > (*owner)->submitted()) return false;
        }
        return true;
    }

    RenderResult<void> DeviceSession::releaseCompleted(BufferPermit& permit, const SubmissionRequirements& uses) noexcept
    {
        auto* value = borrow(permit);
        const bool is_wrong_owner = !value || !owns(uses);
        if (is_wrong_owner) return cxx::unexpected(RenderError{kWrongOwner});
        if (!uses.completed()) return cxx::unexpected(RenderError{kBusy});
        value->retired_ = true;
        permit.owner_ = nullptr;
        permit.key_ = {};
        destroyFinished(value);
        return {};
    }

    RenderResult<void> DeviceSession::releaseCompleted(ImagePermit& permit, const SubmissionRequirements& uses) noexcept
    {
        auto* value = borrow(permit);
        const bool is_wrong_owner = !value || !owns(uses);
        if (is_wrong_owner) return cxx::unexpected(RenderError{kWrongOwner});
        if (!uses.completed()) return cxx::unexpected(RenderError{kBusy});
        value->retired_ = true;
        permit.owner_ = nullptr;
        permit.key_ = {};
        destroyFinished(value);
        return {};
    }

    bool DeviceSession::owns(const Buffer* value) const noexcept
    {
        const auto* entry = value ? buffers_.find(value->key_) : nullptr;
        return entry && entry->get() == value;
    }

    bool DeviceSession::owns(const Image* value) const noexcept
    {
        const auto* entry = value ? images_.find(value->key_) : nullptr;
        return entry && entry->get() == value;
    }

    BufferMemoryBinding Buffer::memoryBinding() const noexcept
    {
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(allocator_->native(), allocation_, &info);
        return {info.deviceMemory, info.offset, info.size};
    }

    ImageMemoryBinding Image::memoryBinding() const noexcept
    {
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(memory_->allocator_->native(), memory_->allocation_, &info);
        return {info.deviceMemory, info.offset, info.size, info.memoryType};
    }

    void DeviceSession::destroyFinished(Buffer* buffer) noexcept
    {
        buffers_.erase(buffer->key_);
    }

    void DeviceSession::destroyFinished(Image* image) noexcept
    {
        images_.erase(image->key_);
    }

} // namespace lux::render::vulkan
