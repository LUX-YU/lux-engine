#pragma once

#include <cstddef>
#include <exception>
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

    class BufferAllocation
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

        // Stable metadata, borrowed from DeviceSession. VkBuffer is the lightweight carrier.
        BufferAllocation(VkBuffer buffer, VmaAllocation_T* allocation, void* mapped,
            VkDeviceSize size, VkBufferUsageFlags usage, EMemoryAccess access) noexcept
            : buffer_(buffer), allocation_(allocation), mapped_(mapped), size_(size), usage_(usage), access_(access) {}

        BufferAllocation(const BufferAllocation&) = delete;
        BufferAllocation& operator=(const BufferAllocation&) = delete;

    private:
        friend class DeviceSession;
        friend class RetirementQueue;
        cxx::SlotKey<BufferAllocationTag> key_;
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
        bool owns_allocation;
    };

    // Optimal tiled 2D backing. Views borrow it; array layers and mip levels
    // belong to this allocation, not to independent per-mip resource owners.
    class ImageAllocation
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

        ImageAllocation(VkImage image, VmaAllocation_T* allocation, ImageDescription description,
            ImageAllocation* source = nullptr) noexcept
            : image_(image), allocation_(allocation), description_(description), source_(source) {}

        ImageAllocation(const ImageAllocation&) = delete;
        ImageAllocation& operator=(const ImageAllocation&) = delete;

    private:
        friend class DeviceSession;
        friend class RetirementQueue;
        cxx::SlotKey<ImageAllocationTag> key_;
        VkImage image_;
        VmaAllocation_T* allocation_;
        ImageDescription description_;
        ImageAllocation* source_;
        std::uint32_t aliases_ = 0;
        bool released_ = false;
        bool retired_ = false;
    };
}
