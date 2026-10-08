#pragma once
/**
 * @file VmaTypes.hpp
 * @brief Move-only RAII owners for VMA allocators, pools, buffers and images.
 *
 * Each owner retains its native resource and the context required to destroy it.
 * Buffer and image owners also provide mapping and visibility helpers.
 * reset() permits early release at the resource's existing safe point.
 *
 * Intended for all general-purpose VMA allocations.
 */
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/gpu/VmaFwd.hpp>
#include <utility>
#include <vulkan/vulkan.h>

namespace lux::render
{
    /// Owns a VMA allocator. Its native state retains the creation device and allocation callbacks;
    /// the instance/device and callback context must outlive it, and all allocations must retire first.
    class LUX_FUNCTION_PUBLIC VmaAllocatorOwner final
    {
    public:
        using CreateResult = lux::cxx::expected<VmaAllocatorOwner, VkResult>;

        [[nodiscard]] static CreateResult create(const VmaAllocatorCreateInfo& info) noexcept;

        VmaAllocatorOwner() noexcept = default;
        ~VmaAllocatorOwner() noexcept;

        VmaAllocatorOwner(const VmaAllocatorOwner&) = delete;
        VmaAllocatorOwner& operator=(const VmaAllocatorOwner&) = delete;
        VmaAllocatorOwner(VmaAllocatorOwner&& other) noexcept;
        VmaAllocatorOwner& operator=(VmaAllocatorOwner&& other) noexcept;

        [[nodiscard]] VmaAllocator get() const noexcept
        {
            return allocator_;
        }

        explicit operator bool() const noexcept
        {
            return allocator_ != nullptr;
        }

        void reset() noexcept;

    private:
        explicit VmaAllocatorOwner(VmaAllocator allocator) noexcept : allocator_(allocator) {}

        VmaAllocator allocator_{};
    };

    /// Owns one custom pool. Its allocator is borrowed and must outlive it;
    /// all allocations from the pool must have retired before it is destroyed.
    class LUX_FUNCTION_PUBLIC VmaPoolOwner final
    {
    public:
        using CreateResult = lux::cxx::expected<VmaPoolOwner, VkResult>;

        [[nodiscard]] static CreateResult create(VmaAllocator allocator, const VmaPoolCreateInfo& info) noexcept;

        VmaPoolOwner() noexcept = default;
        ~VmaPoolOwner() noexcept;

        VmaPoolOwner(const VmaPoolOwner&) = delete;
        VmaPoolOwner& operator=(const VmaPoolOwner&) = delete;
        VmaPoolOwner(VmaPoolOwner&& other) noexcept;
        VmaPoolOwner& operator=(VmaPoolOwner&& other) noexcept;

        [[nodiscard]] VmaPool get() const noexcept
        {
            return pool_;
        }

        explicit operator bool() const noexcept
        {
            return pool_ != nullptr;
        }

        void reset() noexcept;

    private:
        VmaPoolOwner(VmaAllocator allocator, VmaPool pool) noexcept : allocator_(allocator), pool_(pool) {}

        VmaAllocator allocator_{};
        VmaPool pool_{};
    };

    // =====================================================================
    //  VmaBuffer — move-only RAII wrapper for VMA-allocated VkBuffer
    // =====================================================================

    class LUX_FUNCTION_PUBLIC VmaBuffer
    {
    public:
        VmaBuffer() = default;

        struct Allocation
        {
            VmaAllocator allocator{};
            VkBuffer buffer{};
            VmaAllocation allocation{};
        };

        [[nodiscard]] static VmaBuffer adopt(Allocation allocation) noexcept
        {
            VmaBuffer result;
            result.allocator_ = allocation.allocator;
            result.buffer_ = allocation.buffer;
            result.allocation_ = allocation.allocation;
            return result;
        }

        [[nodiscard]] Allocation release() noexcept
        {
            return {std::exchange(allocator_, {}), std::exchange(buffer_, {}), std::exchange(allocation_, {})};
        }

        [[nodiscard]] static Expected<VmaBuffer> create(
            VmaAllocator allocator,
            const VkBufferCreateInfo& buffer_info,
            const VmaAllocationCreateInfo& allocation_info
        );

        ~VmaBuffer();

        VmaBuffer(VmaBuffer&& o) noexcept;
        VmaBuffer& operator=(VmaBuffer&& o) noexcept;

        VmaBuffer(const VmaBuffer&) = delete;
        VmaBuffer& operator=(const VmaBuffer&) = delete;

        [[nodiscard]] VkBuffer buffer() const noexcept
        {
            return buffer_;
        }
        [[nodiscard]] VmaAllocation allocation() const noexcept
        {
            return allocation_;
        }
        [[nodiscard]] bool valid() const noexcept
        {
            return buffer_ != VK_NULL_HANDLE;
        }
        explicit operator bool() const noexcept
        {
            return valid();
        }

        /// Map the allocation for CPU access.  Returns nullptr on failure.
        [[nodiscard]] void* map();
        void unmap();
        void flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

        /// Explicitly release the allocation (idempotent).
        void reset() noexcept;

    private:
        VmaAllocator allocator_{VK_NULL_HANDLE};
        VkBuffer buffer_{VK_NULL_HANDLE};
        VmaAllocation allocation_{VK_NULL_HANDLE};
    };

    // =====================================================================
    //  VmaImage — move-only RAII wrapper for VMA-allocated VkImage
    // =====================================================================

    class LUX_FUNCTION_PUBLIC VmaImage
    {
    public:
        VmaImage() = default;

        struct Allocation
        {
            VmaAllocator allocator{};
            VkImage image{};
            VmaAllocation allocation{};
        };

        [[nodiscard]] static VmaImage adopt(Allocation allocation) noexcept
        {
            VmaImage result;
            result.allocator_ = allocation.allocator;
            result.image_ = allocation.image;
            result.allocation_ = allocation.allocation;
            return result;
        }

        /// Transfer the native allocation into the original runtime's retirement owner.
        [[nodiscard]] Allocation release() noexcept
        {
            return {std::exchange(allocator_, {}), std::exchange(image_, {}), std::exchange(allocation_, {})};
        }

        [[nodiscard]] static Expected<VmaImage> create(
            VmaAllocator allocator,
            const VkImageCreateInfo& image_info,
            const VmaAllocationCreateInfo& allocation_info
        );

        ~VmaImage();

        VmaImage(VmaImage&& o) noexcept;
        VmaImage& operator=(VmaImage&& o) noexcept;

        VmaImage(const VmaImage&) = delete;
        VmaImage& operator=(const VmaImage&) = delete;

        [[nodiscard]] VkImage image() const noexcept
        {
            return image_;
        }
        [[nodiscard]] VmaAllocation allocation() const noexcept
        {
            return allocation_;
        }
        [[nodiscard]] bool valid() const noexcept
        {
            return image_ != VK_NULL_HANDLE;
        }
        explicit operator bool() const noexcept
        {
            return valid();
        }

        /// Explicitly release the allocation (idempotent).
        void reset() noexcept;

    private:
        VmaAllocator allocator_{VK_NULL_HANDLE};
        VkImage image_{VK_NULL_HANDLE};
        VmaAllocation allocation_{VK_NULL_HANDLE};
    };

} // namespace lux::render
