#pragma once

// Optional, header-only VMA carriers. The consumer supplies exactly one VMA implementation.
// These values do not retain an allocator and do not destroy their allocation. Their actual
// collection owner must release them, or transfer them to an operation that will do so.
#if defined(LUX_GAPI_ENABLE_VMA) && LUX_GAPI_ENABLE_VMA

#include <cstddef>
#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <span>
#include <utility>
#include <vk_mem_alloc.h>

namespace lux::gapi::vk
{
    class VmaBuffer final
    {
    public:
        VmaBuffer() noexcept = default;
        VmaBuffer(const VmaBuffer&) = delete;
        VmaBuffer& operator=(const VmaBuffer&) = delete;

        VmaBuffer(VmaBuffer&& other) noexcept
            : buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
              allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)), size_(std::exchange(other.size_, 0)),
              usage_(std::exchange(other.usage_, 0))
        {
        }

        // As with the other gapi carriers, the destination must already be empty.
        VmaBuffer& operator=(VmaBuffer&& other) noexcept
        {
            if (this != &other)
            {
                buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
                allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
                size_ = std::exchange(other.size_, 0);
                usage_ = std::exchange(other.usage_, 0);
            }
            return *this;
        }

        [[nodiscard]] static cxx::expected<VmaBuffer, VkResult> create(
            VmaAllocator allocator,
            const VkBufferCreateInfo& info,
            const VmaAllocationCreateInfo& memory
        ) noexcept
        {
            VmaBuffer result;
            const auto status =
                vmaCreateBuffer(allocator, &info, &memory, &result.buffer_, &result.allocation_, nullptr);
            if (status != VK_SUCCESS)
            {
                return cxx::unexpected(status);
            }
            result.size_ = info.size;
            result.usage_ = info.usage;
            return result;
        }

        void release(VmaAllocator allocator) noexcept
        {
            if (buffer_)
            {
                vmaDestroyBuffer(
                    allocator,
                    std::exchange(buffer_, VK_NULL_HANDLE),
                    std::exchange(allocation_, VK_NULL_HANDLE)
                );
            }
            size_ = 0;
            usage_ = 0;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return buffer_ != VK_NULL_HANDLE;
        }

        [[nodiscard]] VkBuffer handle() const noexcept
        {
            return buffer_;
        }

        [[nodiscard]] VmaAllocation allocation() const noexcept
        {
            return allocation_;
        }

        [[nodiscard]] VkDeviceSize size() const noexcept
        {
            return size_;
        }

        [[nodiscard]] VkBufferUsageFlags usage() const noexcept
        {
            return usage_;
        }

        [[nodiscard]] std::span<std::byte> mappedBytes(VmaAllocator allocator) const noexcept
        {
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(allocator, allocation_, &info);
            return info.pMappedData ? std::span(static_cast<std::byte*>(info.pMappedData), std::size_t(size_))
                                    : std::span<std::byte>{};
        }

        [[nodiscard]] VkResult flush(VmaAllocator allocator, VkDeviceSize offset, VkDeviceSize bytes) const noexcept
        {
            return vmaFlushAllocation(allocator, allocation_, offset, bytes);
        }

        [[nodiscard]] VkResult invalidate(VmaAllocator allocator, VkDeviceSize offset, VkDeviceSize bytes)
            const noexcept
        {
            return vmaInvalidateAllocation(allocator, allocation_, offset, bytes);
        }

    private:
        VkBuffer buffer_ = VK_NULL_HANDLE;
        VmaAllocation allocation_ = VK_NULL_HANDLE;
        VkDeviceSize size_ = 0;
        VkBufferUsageFlags usage_ = 0;
    };

    class VmaImage final
    {
    public:
        VmaImage() noexcept = default;
        VmaImage(const VmaImage&) = delete;
        VmaImage& operator=(const VmaImage&) = delete;

        VmaImage(VmaImage&& other) noexcept
            : image_(std::exchange(other.image_, VK_NULL_HANDLE)),
              allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)), extent_(other.extent_),
              format_(other.format_), usage_(other.usage_), samples_(other.samples_), mip_levels_(other.mip_levels_),
              array_layers_(other.array_layers_)
        {
        }

        // Release the destination through its collection owner before replacing it.
        VmaImage& operator=(VmaImage&& other) noexcept
        {
            if (this != &other)
            {
                image_ = std::exchange(other.image_, VK_NULL_HANDLE);
                allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
                extent_ = other.extent_;
                format_ = other.format_;
                usage_ = other.usage_;
                samples_ = other.samples_;
                mip_levels_ = other.mip_levels_;
                array_layers_ = other.array_layers_;
            }
            return *this;
        }

        [[nodiscard]] static cxx::expected<VmaImage, VkResult> create(
            VmaAllocator allocator,
            const VkImageCreateInfo& info,
            const VmaAllocationCreateInfo& memory
        ) noexcept
        {
            VmaImage result;
            const auto status = vmaCreateImage(allocator, &info, &memory, &result.image_, &result.allocation_, nullptr);
            if (status != VK_SUCCESS)
            {
                return cxx::unexpected(status);
            }
            result.extent_ = info.extent;
            result.format_ = info.format;
            result.usage_ = info.usage;
            result.samples_ = info.samples;
            result.mip_levels_ = info.mipLevels;
            result.array_layers_ = info.arrayLayers;
            return result;
        }

        void release(VmaAllocator allocator) noexcept
        {
            if (image_)
            {
                vmaDestroyImage(
                    allocator,
                    std::exchange(image_, VK_NULL_HANDLE),
                    std::exchange(allocation_, VK_NULL_HANDLE)
                );
            }
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return image_ != VK_NULL_HANDLE;
        }

        [[nodiscard]] VkImage handle() const noexcept
        {
            return image_;
        }

        [[nodiscard]] VmaAllocation allocation() const noexcept
        {
            return allocation_;
        }

        [[nodiscard]] VkExtent3D extent() const noexcept
        {
            return extent_;
        }

        [[nodiscard]] VkFormat format() const noexcept
        {
            return format_;
        }

        [[nodiscard]] VkImageUsageFlags usage() const noexcept
        {
            return usage_;
        }

        [[nodiscard]] VkSampleCountFlagBits samples() const noexcept
        {
            return samples_;
        }

        [[nodiscard]] std::uint32_t mipLevels() const noexcept
        {
            return mip_levels_;
        }

        [[nodiscard]] std::uint32_t arrayLayers() const noexcept
        {
            return array_layers_;
        }

    private:
        VkImage image_ = VK_NULL_HANDLE;
        VmaAllocation allocation_ = VK_NULL_HANDLE;
        VkExtent3D extent_{};
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage_ = 0;
        VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
        std::uint32_t mip_levels_ = 0;
        std::uint32_t array_layers_ = 0;
    };
} // namespace lux::gapi::vk

#endif
