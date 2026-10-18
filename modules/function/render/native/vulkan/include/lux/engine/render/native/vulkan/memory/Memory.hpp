#pragma once

#include <cstddef>
#include <exception>
#include <memory>
#include <utility>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/native/vulkan/device/Device.hpp>

struct VmaAllocation_T;

namespace lux::render::vulkan
{
    enum class EMemoryAccess
    {
        DEVICE,
        UPLOAD,
        READBACK
    };

    struct BufferMemoryBinding
    {
        VkDeviceMemory memory;
        VkDeviceSize offset, size;
    };

    struct BufferAllocationTag;
    struct ImageAllocationTag;
    class DeviceSession;
    // A cold, exclusive release qualification. The Session must outlive it and all record borrows.
    // Dropping a permit leaves reclamation to Session shutdown; it never destroys GPU backing implicitly.
    template <class Tag>
    class AllocationPermit final
    {
    public:
        AllocationPermit() noexcept = default;
        AllocationPermit(AllocationPermit&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), key_(std::exchange(other.key_, {})) {}

        AllocationPermit& operator=(AllocationPermit&& other) noexcept
        {
            if (this != &other)
            {
                // An outstanding release qualification must not silently disappear on assignment.
                if (owner_) std::terminate();
                owner_ = std::exchange(other.owner_, nullptr);
                key_ = std::exchange(other.key_, {});
            }
            return *this;
        }

        AllocationPermit(const AllocationPermit&) = delete;
        AllocationPermit& operator=(const AllocationPermit&) = delete;
        [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }

    private:
        friend class DeviceSession;
        friend class RetirementQueue;
        friend struct NativeMemoryAudit;
        AllocationPermit(DeviceSession& owner, cxx::SlotKey<Tag> key) noexcept : owner_(&owner), key_(key) {}
        DeviceSession* owner_ = nullptr;
        cxx::SlotKey<Tag> key_{};
    };
    using BufferPermit = AllocationPermit<BufferAllocationTag>;
    using ImagePermit = AllocationPermit<ImageAllocationTag>;

    class Buffer
    {
    public:
        [[nodiscard]] VkBuffer native() const noexcept
        {
            return buffer_;
        }

        [[nodiscard]] VkDeviceSize size() const noexcept
        {
            return size_;
        }

        [[nodiscard]] EMemoryAccess access() const noexcept
        {
            return access_;
        }

        [[nodiscard]] VkBufferUsageFlags usage() const noexcept
        {
            return usage_;
        }

        // Owns VkBuffer and its allocation. The allocator outlives this object; GPU users retain it.
        Buffer(
            VulkanAllocator& allocator,
            VkBuffer buffer,
            VmaAllocation_T* allocation,
            void* mapped,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            EMemoryAccess access
        ) noexcept
            : allocator_(&allocator), buffer_(buffer), allocation_(allocation), mapped_(mapped),
              size_(size), usage_(usage), access_(access)
        {
        }

        ~Buffer() noexcept;

        [[nodiscard]] RenderResult<void> write(VkDeviceSize offset, std::span<const std::byte> bytes) noexcept;
        [[nodiscard]] RenderResult<void> read(VkDeviceSize offset, std::span<std::byte> bytes) const noexcept;
        [[nodiscard]] BufferMemoryBinding memoryBinding() const noexcept;

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

    private:
        friend class VulkanAllocator;
        friend class DeviceSession;
        friend class RetirementQueue;
        cxx::SlotKey<BufferAllocationTag> key_;
        friend class Transfer;
        VulkanAllocator* allocator_;
        VkBuffer buffer_;
        VmaAllocation_T* allocation_;
        void* mapped_;
        VkDeviceSize size_;
        VkBufferUsageFlags usage_;
        EMemoryAccess access_;
        bool retired_ = false;
    };

    struct ImageDescription
    {
        VkExtent2D extent;
        VkFormat format;
        VkImageUsageFlags usage;
        VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
        std::uint32_t mip_levels{1};
        std::uint32_t array_layers{1};
        bool aliasable{false};
    };

    struct ImageMemoryBinding
    {
        VkDeviceMemory memory;
        VkDeviceSize offset, size;
        std::uint32_t memory_type;
    };

    // Actual VMA allocation shared by alias Images. No Image points at another Image.
    class ImageMemory final
    {
    public:
        ImageMemory(VulkanAllocator& allocator, VmaAllocation_T* allocation) noexcept
            : allocator_(&allocator), allocation_(allocation)
        {
        }

        ~ImageMemory() noexcept;
        ImageMemory(const ImageMemory&) = delete;
        ImageMemory& operator=(const ImageMemory&) = delete;

    private:
        friend class VulkanAllocator;
        friend class DeviceSession;
        friend class Image;
        VulkanAllocator* allocator_;
        VmaAllocation_T* allocation_;
    };

    // Optimal tiled 2D backing. Views borrow it; array layers and mip levels
    // belong to this allocation, not to independent per-mip resource owners.
    class Image
    {
    public:
        [[nodiscard]] VkImage native() const noexcept
        {
            return image_;
        }

        [[nodiscard]] VkExtent2D extent() const noexcept
        {
            return description_.extent;
        }

        [[nodiscard]] VkFormat format() const noexcept
        {
            return description_.format;
        }

        [[nodiscard]] VkImageUsageFlags usage() const noexcept
        {
            return description_.usage;
        }

        [[nodiscard]] VkSampleCountFlagBits samples() const noexcept
        {
            return description_.samples;
        }

        [[nodiscard]] const ImageDescription& description() const noexcept
        {
            return description_;
        }

        Image(VkImage image, std::shared_ptr<ImageMemory> memory, ImageDescription description) noexcept
            : image_(image), memory_(std::move(memory)), description_(description)
        {
        }

        ~Image() noexcept;

        [[nodiscard]] ImageMemoryBinding memoryBinding() const noexcept;

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

    private:
        friend class VulkanAllocator;
        friend class DeviceSession;
        friend class RetirementQueue;
        cxx::SlotKey<ImageAllocationTag> key_;
        VkImage image_;
        std::shared_ptr<ImageMemory> memory_;
        ImageDescription description_;
        bool retired_ = false;
    };
}
