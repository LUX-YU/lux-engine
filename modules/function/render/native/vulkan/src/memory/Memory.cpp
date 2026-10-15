#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

#include "Native.hpp"
#include <algorithm>
#include <cstring>
#include <utility>
#include <vk_mem_alloc.h>

namespace lux::render::vulkan
{
    RenderResult<BufferAllocation*> DeviceSession::createBufferRecord(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        EMemoryAccess access
    ) noexcept
    {
        const bool is_invalid_access =
            access != EMemoryAccess::DEVICE && access != EMemoryAccess::UPLOAD && access != EMemoryAccess::READBACK;
        const bool is_invalid_config = size == 0 || usage == 0 || is_invalid_access;
        if (is_invalid_config)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
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
            LUX_NATIVE("buffer", vmaCreateBuffer(allocator_.native(), &info, &memory, &buffer, &allocation, &mapped));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        if (access != EMemoryAccess::DEVICE)
        {
            const auto mapping = LUX_NATIVE("mapping", mapped.pMappedData ? VK_SUCCESS : VK_ERROR_MEMORY_MAP_FAILED);
            if (mapping != VK_SUCCESS)
            {
                LUX_DESTROY("buffer", vmaDestroyBuffer(allocator_.native(), buffer, allocation));
                return cxx::unexpected(nativeError(mapping));
            }
        }
        const auto key = buffers_.emplace(buffer, allocation, mapped.pMappedData, size, usage, access);
        auto* record = buffers_.find(key);
        record->key_ = key;
        return record;
    }

    RenderResult<void> DeviceSession::write(BufferAllocation& buffer, VkDeviceSize offset,
        std::span<const std::byte> bytes) noexcept
    {
        if (!owns(&buffer)) return cxx::unexpected(RenderError{kWrongOwner});
        const bool is_invalid_range = offset > buffer.size_ || bytes.size() > buffer.size_ - offset;
        const bool is_invalid_access = buffer.access_ == EMemoryAccess::DEVICE || is_invalid_range;
        if (is_invalid_access)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (bytes.empty())
        {
            return {};
        }
        std::memcpy(static_cast<std::byte*>(buffer.mapped_) + offset, bytes.data(), bytes.size());
        const auto result = LUX_NATIVE("flush",
            vmaFlushAllocation(allocator_.native(), buffer.allocation_, offset, bytes.size())
        );
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        return {};
    }

    RenderResult<void> DeviceSession::read(const BufferAllocation& buffer, VkDeviceSize offset,
        std::span<std::byte> bytes) const noexcept
    {
        if (!owns(&buffer)) return cxx::unexpected(RenderError{kWrongOwner});
        const bool is_invalid_range = offset > buffer.size_ || bytes.size() > buffer.size_ - offset;
        const bool is_invalid_access = buffer.access_ != EMemoryAccess::READBACK || is_invalid_range;
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
                vmaInvalidateAllocation(allocator_.native(), buffer.allocation_, offset, bytes.size())
            );
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        std::memcpy(bytes.data(), static_cast<const std::byte*>(buffer.mapped_) + offset, bytes.size());
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

    RenderResult<ImageAllocation*> DeviceSession::createImageRecord(const ImageDescription& description) noexcept
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
        vmaGetAllocatorInfo(allocator_.native(), &allocator_info);
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
            LUX_NATIVE("image", vmaCreateImage(allocator_.native(), &info, &memory, &image, &allocation, nullptr));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        const auto key = images_.emplace(image, allocation, description);
        auto* record = images_.find(key);
        record->key_ = key;
        return record;
    }

    RenderResult<ImagePermit> DeviceSession::aliasImage(const ImagePermit& permit) noexcept
    {
        auto* record = borrow(permit);
        if (!record) return cxx::unexpected(RenderError{kWrongOwner});
        auto& source = *record;
        const bool is_invalid_source = !source.description_.aliasable || source.source_ || source.released_;
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
        auto created = LUX_NATIVE("image_alias", vkCreateImage(device_.native(), &info, nullptr, &image));
        if (created != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(created));
        }

        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        requirements.pNext = &dedicated;
        VkImageMemoryRequirementsInfo2 query{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        query.image = image;
        vkGetImageMemoryRequirements2(device_.native(), &query, &requirements);
        const auto binding = memoryBinding(source);
        const auto& required = requirements.memoryRequirements;
        const bool incompatible = dedicated.requiresDedicatedAllocation || required.size > binding.size ||
                                  (required.memoryTypeBits & (1u << binding.memory_type)) == 0 ||
                                  binding.offset % required.alignment != 0;
        if (incompatible)
        {
            LUX_DESTROY("image_alias", vkDestroyImage(device_.native(), image, nullptr));
            return cxx::unexpected(RenderError{kUnsupported});
        }
        created = LUX_NATIVE("image_alias_bind", vmaBindImageMemory(allocator_.native(), source.allocation_, image));
        if (created != VK_SUCCESS)
        {
            LUX_DESTROY("image_alias", vkDestroyImage(device_.native(), image, nullptr));
            return cxx::unexpected(nativeError(created));
        }
        const auto key = images_.emplace(image, source.allocation_, description, &source);
        images_.find(key)->key_ = key;
        ++source.aliases_;
        return ImagePermit{*this, key};
    }
    RenderResult<BufferPermit> DeviceSession::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, EMemoryAccess access) noexcept
    {
        auto record = createBufferRecord(size, usage, access);
        if (!record) return cxx::unexpected(record.error());
        return BufferPermit{*this, (*record)->key_};
    }

    BufferAllocation* DeviceSession::borrow(const BufferPermit& permit) const noexcept
    {
        if (permit.owner_ != this) return nullptr;
        auto* value = const_cast<DeviceSession*>(this)->buffers_.find(permit.key_);
        return value && !value->retired_ ? value : nullptr;
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

    ImageAllocation* DeviceSession::borrow(const ImagePermit& permit) const noexcept
    {
        if (permit.owner_ != this) return nullptr;
        auto* value = const_cast<DeviceSession*>(this)->images_.find(permit.key_);
        return value && !value->retired_ && !value->released_ ? value : nullptr;
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

    bool DeviceSession::owns(const BufferAllocation* value) const noexcept
    {
        return value && buffers_.find(value->key_) == value;
    }

    bool DeviceSession::owns(const ImageAllocation* value) const noexcept
    {
        return value && images_.find(value->key_) == value;
    }

    BufferMemoryBinding DeviceSession::memoryBinding(const BufferAllocation& buffer) const noexcept
    {
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(allocator_.native(), buffer.allocation_, &info);
        return {info.deviceMemory, info.offset, info.size};
    }

    ImageMemoryBinding DeviceSession::memoryBinding(const ImageAllocation& image) const noexcept
    {
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(allocator_.native(), image.allocation_, &info);
        return {info.deviceMemory, info.offset, info.size, info.memoryType, image.source_ == nullptr};
    }

    void DeviceSession::destroyFinished(BufferAllocation* buffer) noexcept
    {
        const auto key = buffer->key_;
        LUX_DESTROY("buffer", vmaDestroyBuffer(allocator_.native(), buffer->buffer_, buffer->allocation_));
        buffers_.erase(key);
    }

    void DeviceSession::destroyFinished(ImageAllocation* image) noexcept
    {
        // Aliases pin the VMA allocation even if its original image is released first.
        if (image->aliases_) { image->released_ = true; return; }
        auto* source = image->source_;
        const auto key = image->key_;
        LUX_DESTROY(source ? "image_alias" : "image",
            vmaDestroyImage(allocator_.native(), image->image_, source ? nullptr : image->allocation_));
        images_.erase(key);
        if (source && --source->aliases_ == 0 && source->released_) destroyFinished(source);
    }

} // namespace lux::render::vulkan
