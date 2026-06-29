#pragma once
/**
 * @file VmaTypes.hpp
 * @brief Move-only RAII wrappers for VMA-allocated buffers and images.
 *
 * These types own a single VMA allocation and automatically destroy it
 * in the destructor.  They provide map/unmap/flush helpers and an
 * explicit reset() for early release.
 *
 * Intended for all general-purpose VMA allocations.
 */
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/core/VmaFwd.hpp>
#include <vulkan/vulkan.h>

namespace lux::render
{

    // =====================================================================
    //  VmaBuffer — move-only RAII wrapper for VMA-allocated VkBuffer
    // =====================================================================

    class LUX_FUNCTION_PUBLIC VmaBuffer
    {
    public:
        VmaBuffer() = default;

        /// Allocate a buffer immediately.  Throws on failure.
        VmaBuffer(VmaAllocator alloc,
                  const VkBufferCreateInfo& bci,
                  const VmaAllocationCreateInfo& aci);

        ~VmaBuffer();

        VmaBuffer(VmaBuffer&& o) noexcept;
        VmaBuffer& operator=(VmaBuffer&& o) noexcept;

        VmaBuffer(const VmaBuffer&) = delete;
        VmaBuffer& operator=(const VmaBuffer&) = delete;

        [[nodiscard]] VkBuffer      buffer()     const noexcept { return buffer_; }
        [[nodiscard]] VmaAllocation allocation() const noexcept { return allocation_; }
        [[nodiscard]] bool          valid()      const noexcept { return buffer_ != VK_NULL_HANDLE; }
        explicit operator bool()                 const noexcept { return valid(); }

        /// Map the allocation for CPU access.  Returns nullptr on failure.
        [[nodiscard]] void* map();
        void unmap();
        void flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

        /// Explicitly release the allocation (idempotent).
        void reset() noexcept;

    private:
        VmaAllocator  allocator_  {VK_NULL_HANDLE};
        VkBuffer      buffer_     {VK_NULL_HANDLE};
        VmaAllocation allocation_ {VK_NULL_HANDLE};
    };

    // =====================================================================
    //  VmaImage — move-only RAII wrapper for VMA-allocated VkImage
    // =====================================================================

    class LUX_FUNCTION_PUBLIC VmaImage
    {
    public:
        VmaImage() = default;

        /// Allocate an image immediately.  Throws on failure.
        VmaImage(VmaAllocator alloc,
                 const VkImageCreateInfo& ici,
                 const VmaAllocationCreateInfo& aci);

        ~VmaImage();

        VmaImage(VmaImage&& o) noexcept;
        VmaImage& operator=(VmaImage&& o) noexcept;

        VmaImage(const VmaImage&) = delete;
        VmaImage& operator=(const VmaImage&) = delete;

        [[nodiscard]] VkImage       image()      const noexcept { return image_; }
        [[nodiscard]] VmaAllocation allocation() const noexcept { return allocation_; }
        [[nodiscard]] bool          valid()      const noexcept { return image_ != VK_NULL_HANDLE; }
        explicit operator bool()                 const noexcept { return valid(); }

        /// Explicitly release the allocation (idempotent).
        void reset() noexcept;

    private:
        VmaAllocator  allocator_  {VK_NULL_HANDLE};
        VkImage       image_      {VK_NULL_HANDLE};
        VmaAllocation allocation_ {VK_NULL_HANDLE};
    };

} // namespace lux::render
