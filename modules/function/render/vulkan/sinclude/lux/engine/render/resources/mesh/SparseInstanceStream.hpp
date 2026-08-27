#pragma once

#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace lux::render
{
    class DeferredDestroyQueue;

    inline constexpr std::uint32_t kInstanceSlotsPerPage = 16'384u;
    inline constexpr std::uint32_t kInstancePageOffsetBits = 14u;
    inline constexpr std::uint32_t kInstancePageTableAxisBits = 9u;
    inline constexpr std::uint32_t kInstancePageTableAxisSize = 512u;

    [[nodiscard]] inline constexpr std::uint32_t instancePageOffset(std::uint32_t slot) noexcept
    {
        return slot & (kInstanceSlotsPerPage - 1u);
    }

    [[nodiscard]] inline constexpr std::uint32_t instancePageIndex(std::uint32_t slot) noexcept
    {
        return slot >> kInstancePageOffsetBits;
    }

    [[nodiscard]] inline constexpr std::uint32_t instanceRootIndex(std::uint32_t slot) noexcept
    {
        return instancePageIndex(slot) >> kInstancePageTableAxisBits;
    }

    [[nodiscard]] inline constexpr std::uint32_t instanceLeafIndex(std::uint32_t slot) noexcept
    {
        return instancePageIndex(slot) & (kInstancePageTableAxisSize - 1u);
    }

    struct alignas(8) GpuInstancePageAddresses final
    {
        VkDeviceAddress transform{0u};
        VkDeviceAddress previous_transform{0u};
        VkDeviceAddress property{0u};
        VkDeviceAddress cull_meta{0u};
    };
    static_assert(sizeof(GpuInstancePageAddresses) == 32u);

    /// Fixed root plus on-demand leaves. Root entries contain leaf BDAs; each
    /// leaf entry contains the four field-page BDAs for one 16K-slot page.
    class LUX_FUNCTION_PUBLIC SparseInstancePageTable final
    {
    public:
        SparseInstancePageTable() = default;
        ~SparseInstancePageTable();
        SparseInstancePageTable(const SparseInstancePageTable&) = delete;
        SparseInstancePageTable& operator=(const SparseInstancePageTable&) = delete;

        [[nodiscard]] bool init(DeviceContext* device_context);
        void shutdown();
        [[nodiscard]] bool publish(std::uint32_t page_index, const GpuInstancePageAddresses& addresses);
        [[nodiscard]] VkBuffer rootBuffer() const noexcept
        {
            return root_buffer_;
        }
        [[nodiscard]] VkDeviceSize rootBufferBytes() const noexcept
        {
            return sizeof(VkDeviceAddress) * kInstancePageTableAxisSize;
        }
        [[nodiscard]] std::uint32_t leafCount() const noexcept;
        void setDeferredQueue(DeferredDestroyQueue* queue) noexcept
        {
            deferred_queue_ = queue;
        }

    private:
        struct Leaf final
        {
            VkBuffer buffer{VK_NULL_HANDLE};
            VmaAllocation allocation{nullptr};
            GpuInstancePageAddresses* mapped{nullptr};
            VkDeviceAddress address{0u};
        };

        [[nodiscard]] bool createLeaf(std::uint32_t root_index);
        void destroyBuffer(VkBuffer buffer, VmaAllocation allocation);

        DeviceContext* device_context_{nullptr};
        DeferredDestroyQueue* deferred_queue_{nullptr};
        VkBuffer root_buffer_{VK_NULL_HANDLE};
        VmaAllocation root_allocation_{nullptr};
        VkDeviceAddress* root_mapped_{nullptr};
        std::vector<Leaf> leaves_;
    };

    /// Type-erased backing for one independently paged instance field.
    class LUX_FUNCTION_PUBLIC SparseInstanceStreamStorage final
    {
    public:
        struct UploadChunk final
        {
            const std::byte* src{nullptr};
            VkBuffer destination{VK_NULL_HANDLE};
            VkDeviceSize destination_offset{0u};
            VkDeviceSize size{0u};
        };

        SparseInstanceStreamStorage() = default;
        ~SparseInstanceStreamStorage();
        SparseInstanceStreamStorage(const SparseInstanceStreamStorage&) = delete;
        SparseInstanceStreamStorage& operator=(const SparseInstanceStreamStorage&) = delete;

        [[nodiscard]] bool
        init(DeviceContext* device_context, std::uint32_t stride, std::uint32_t initial_capacity, bool sparse_bda);
        void shutdown();
        [[nodiscard]] bool reserve(std::uint32_t new_capacity);
        void rollbackPages(std::uint32_t page_count);

        [[nodiscard]] std::byte* at(std::uint32_t index) noexcept;
        [[nodiscard]] const std::byte* at(std::uint32_t index) const noexcept;
        void markDirty(std::uint32_t index);
        [[nodiscard]] bool hasDirtyPages() const noexcept
        {
            return !dirty_upload_pages_.empty();
        }
        [[nodiscard]] VkDeviceSize
        collectUploadChunks(std::uint32_t count, bool full_upload, std::vector<UploadChunk>& chunks);
        void clearDirtyState();

        [[nodiscard]] VkBuffer buffer() const noexcept
        {
            return sparse_bda_ ? VK_NULL_HANDLE : flat_buffer_;
        }
        [[nodiscard]] VkBuffer pageBuffer(std::uint32_t page_index) const noexcept;
        [[nodiscard]] VkDeviceAddress pageAddress(std::uint32_t page_index) const noexcept;
        [[nodiscard]] std::uint32_t pageCount() const noexcept
        {
            return static_cast<std::uint32_t>(gpu_pages_.size());
        }
        [[nodiscard]] std::uint32_t capacity() const noexcept
        {
            return capacity_;
        }
        [[nodiscard]] bool sparse() const noexcept
        {
            return sparse_bda_;
        }
        void setDeferredQueue(DeferredDestroyQueue* queue) noexcept
        {
            deferred_queue_ = queue;
        }

    private:
        struct GpuPage final
        {
            VkBuffer buffer{VK_NULL_HANDLE};
            VmaAllocation allocation{nullptr};
            VkDeviceAddress address{0u};
        };

        [[nodiscard]] bool createGpuPage(GpuPage& page);
        [[nodiscard]] bool createFlatBuffer(std::uint32_t capacity);
        void destroyBuffer(VkBuffer buffer, VmaAllocation allocation, bool published);

        static constexpr std::uint32_t kUploadSlotsPerPage = 512u;
        DeviceContext* device_context_{nullptr};
        DeferredDestroyQueue* deferred_queue_{nullptr};
        std::uint32_t stride_{0u};
        std::uint32_t capacity_{0u};
        bool sparse_bda_{false};
        VkBuffer flat_buffer_{VK_NULL_HANDLE};
        VmaAllocation flat_allocation_{nullptr};
        std::vector<std::unique_ptr<std::byte[]>> cpu_pages_;
        std::vector<GpuPage> gpu_pages_;
        std::vector<std::uint32_t> dirty_upload_pages_;
        std::vector<std::uint8_t> dirty_upload_flags_;
    };

    template <class T> class SparseInstanceStream final
    {
    public:
        using UploadChunk = SparseInstanceStreamStorage::UploadChunk;

        [[nodiscard]] bool init(DeviceContext* device_context, std::uint32_t capacity, bool sparse_bda)
        {
            return storage_.init(device_context, sizeof(T), capacity, sparse_bda);
        }
        void shutdown()
        {
            storage_.shutdown();
        }
        [[nodiscard]] bool reserve(std::uint32_t capacity)
        {
            return storage_.reserve(capacity);
        }
        void rollbackPages(std::uint32_t pages)
        {
            storage_.rollbackPages(pages);
        }
        [[nodiscard]] T& at(std::uint32_t index) noexcept
        {
            return *reinterpret_cast<T*>(storage_.at(index));
        }
        [[nodiscard]] const T& at(std::uint32_t index) const noexcept
        {
            return *reinterpret_cast<const T*>(storage_.at(index));
        }
        void markDirty(std::uint32_t index)
        {
            storage_.markDirty(index);
        }
        [[nodiscard]] bool hasDirtyPages() const noexcept
        {
            return storage_.hasDirtyPages();
        }
        [[nodiscard]] VkDeviceSize
        collectUploadChunks(std::uint32_t count, bool full_upload, std::vector<UploadChunk>& chunks)
        {
            return storage_.collectUploadChunks(count, full_upload, chunks);
        }
        void clearDirtyState()
        {
            storage_.clearDirtyState();
        }
        [[nodiscard]] VkBuffer buffer() const noexcept
        {
            return storage_.buffer();
        }
        [[nodiscard]] VkDeviceAddress pageAddress(std::uint32_t page_index) const noexcept
        {
            return storage_.pageAddress(page_index);
        }
        [[nodiscard]] std::uint32_t pageCount() const noexcept
        {
            return storage_.pageCount();
        }
        [[nodiscard]] std::uint32_t capacity() const noexcept
        {
            return storage_.capacity();
        }
        [[nodiscard]] bool sparse() const noexcept
        {
            return storage_.sparse();
        }
        void setDeferredQueue(DeferredDestroyQueue* queue) noexcept
        {
            storage_.setDeferredQueue(queue);
        }

    private:
        SparseInstanceStreamStorage storage_;
    };

    template <class T> class StableInstanceCpuPages final
    {
    public:
        [[nodiscard]] bool reserve(std::uint32_t capacity)
        {
            const auto required_pages = capacity == 0u ? 0u : (capacity - 1u) / kInstanceSlotsPerPage + 1u;
            pages_.reserve(required_pages);
            while (pages_.size() < required_pages)
                pages_.push_back(std::make_unique<T[]>(kInstanceSlotsPerPage));
            return true;
        }
        void clear()
        {
            pages_.clear();
        }
        [[nodiscard]] T& at(std::uint32_t index) noexcept
        {
            return pages_[index >> kInstancePageOffsetBits][index & (kInstanceSlotsPerPage - 1u)];
        }
        [[nodiscard]] const T& at(std::uint32_t index) const noexcept
        {
            return pages_[index >> kInstancePageOffsetBits][index & (kInstanceSlotsPerPage - 1u)];
        }

    private:
        std::vector<std::unique_ptr<T[]>> pages_;
    };
} // namespace lux::render
