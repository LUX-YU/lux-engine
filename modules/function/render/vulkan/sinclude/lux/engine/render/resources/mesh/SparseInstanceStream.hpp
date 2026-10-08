#pragma once

#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/gpu/lifecycle/FifOwned.hpp>
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

namespace lux::render
{
    class DeferredDestroyQueue;
    class DeviceContext;

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
        using CreateResult = Expected<std::unique_ptr<SparseInstancePageTable>>;
        [[nodiscard]] static CreateResult create(DeviceContext& device, DeferredDestroyQueue& retirement) noexcept;

        ~SparseInstancePageTable() noexcept = default;

        SparseInstancePageTable(const SparseInstancePageTable&) = delete;
        SparseInstancePageTable& operator=(const SparseInstancePageTable&) = delete;
        SparseInstancePageTable(SparseInstancePageTable&&) = delete;
        SparseInstancePageTable& operator=(SparseInstancePageTable&&) = delete;

        [[nodiscard]] Expected<void>
        publish(std::uint32_t page_index, const GpuInstancePageAddresses& addresses) noexcept;

        [[nodiscard]] VkBuffer rootBuffer() const noexcept
        {
            return root_buffer_.get();
        }

        [[nodiscard]] static constexpr VkDeviceSize rootBufferBytes() noexcept
        {
            return sizeof(VkDeviceAddress) * kInstancePageTableAxisSize;
        }

        [[nodiscard]] std::uint32_t leafCount() const noexcept;

    private:
        struct Leaf final
        {
            TFifOwnedAllocated<VkBuffer> buffer;
            GpuInstancePageAddresses* mapped{};
        };

        SparseInstancePageTable(
            DeviceContext& device,
            DeferredDestroyQueue& retirement,
            VmaBuffer buffer,
            VkDeviceAddress* mapped
        ) noexcept;

        DeviceContext& device_;
        TFifOwnedAllocated<VkBuffer> root_buffer_;
        VkDeviceAddress* root_mapped_;
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

        [[nodiscard]] static Expected<SparseInstanceStreamStorage>
        create(
            DeviceContext& device,
            DeferredDestroyQueue& retirement,
            std::uint32_t stride,
            std::uint32_t initial_capacity,
            bool sparse_bda
        ) noexcept;

        ~SparseInstanceStreamStorage() noexcept;
        SparseInstanceStreamStorage(const SparseInstanceStreamStorage&) = delete;
        SparseInstanceStreamStorage& operator=(const SparseInstanceStreamStorage&) = delete;
        SparseInstanceStreamStorage(SparseInstanceStreamStorage&&) noexcept = default;
        SparseInstanceStreamStorage& operator=(SparseInstanceStreamStorage&&) = delete;

        [[nodiscard]] Expected<void> reserve(std::uint32_t new_capacity) noexcept;

        [[nodiscard]] std::byte* at(std::uint32_t index) noexcept;
        [[nodiscard]] const std::byte* at(std::uint32_t index) const noexcept;
        void markDirty(std::uint32_t index);
        [[nodiscard]] bool hasDirtyPages() const noexcept
        {
            return !dirty_upload_pages_.empty();
        }

        [[nodiscard]] VkDeviceSize collectUploadChunks(
            std::uint32_t count,
            bool full_upload,
            std::vector<UploadChunk>& chunks
        );
        void clearDirtyState();

        [[nodiscard]] VkBuffer buffer() const noexcept
        {
            return flat_buffer_.get();
        }

        [[nodiscard]] VkBuffer pageBuffer(std::uint32_t page_index) const noexcept;
        [[nodiscard]] VkDeviceAddress pageAddress(std::uint32_t page_index) const noexcept;
        [[nodiscard]] std::uint32_t pageCount() const noexcept
        {
            return static_cast<std::uint32_t>(pages_.size());
        }

        [[nodiscard]] std::uint32_t capacity() const noexcept
        {
            return pageCount() * kInstanceSlotsPerPage;
        }

        [[nodiscard]] bool sparse() const noexcept
        {
            return sparse_bda_;
        }

    private:
        struct Page final
        {
            std::unique_ptr<std::byte[]> cpu;
            VmaBuffer gpu;
            VkDeviceAddress address{};
        };

        template <class T> friend class TSparseInstanceStream;

        // Only the enclosing instance aggregate may span preparation across its four fields.
        // These candidates are unpublished and release native allocations immediately on rejection.
        struct Growth final
        {
            std::vector<Page> pages;
            VmaBuffer flat;
        };

        [[nodiscard]] Expected<Growth> prepareGrowth(std::uint32_t capacity) noexcept;
        void commitGrowth(Growth growth) noexcept;
        [[nodiscard]] VkDeviceAddress pageAddress(const Growth& growth, std::uint32_t index) const noexcept;

        [[nodiscard]] static Expected<std::vector<Page>>
        preparePages(
            DeviceContext& device,
            std::uint32_t stride,
            std::uint32_t count,
            bool sparse_bda
        ) noexcept;

        SparseInstanceStreamStorage(
            DeviceContext& device,
            DeferredDestroyQueue& retirement,
            std::uint32_t stride,
            bool sparse_bda,
            std::vector<Page> pages,
            VmaBuffer flat_buffer
        ) noexcept;

        static constexpr std::uint32_t kUploadSlotsPerPage = 512u;
        DeviceContext& device_;
        DeferredDestroyQueue& retirement_;
        std::uint32_t stride_;
        bool sparse_bda_;
        TFifOwnedAllocated<VkBuffer> flat_buffer_;
        std::vector<Page> pages_;
        std::vector<std::uint32_t> dirty_upload_pages_;
        std::vector<std::uint8_t> dirty_upload_flags_;
    };

    template <class T> class TSparseInstanceStream final
    {
    public:
        using UploadChunk = SparseInstanceStreamStorage::UploadChunk;

        [[nodiscard]] static Expected<TSparseInstanceStream>
        create(
            DeviceContext& device,
            DeferredDestroyQueue& retirement,
            std::uint32_t capacity,
            bool sparse_bda
        ) noexcept
        {
            auto storage = SparseInstanceStreamStorage::create(device, retirement, sizeof(T), capacity, sparse_bda);
            if (!storage)
            {
                return lux::cxx::unexpected(storage.error());
            }
            return TSparseInstanceStream{std::move(*storage)};
        }

        ~TSparseInstanceStream() noexcept = default;
        TSparseInstanceStream(const TSparseInstanceStream&) = delete;
        TSparseInstanceStream& operator=(const TSparseInstanceStream&) = delete;
        TSparseInstanceStream(TSparseInstanceStream&&) noexcept = default;
        TSparseInstanceStream& operator=(TSparseInstanceStream&&) = delete;

        [[nodiscard]] Expected<void> reserve(std::uint32_t capacity) noexcept
        {
            return storage_.reserve(capacity);
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

        [[nodiscard]] VkDeviceSize collectUploadChunks(
            std::uint32_t count,
            bool full_upload,
            std::vector<UploadChunk>& chunks
        )
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

    private:
        friend class InstanceResources;
        using Growth = SparseInstanceStreamStorage::Growth;

        [[nodiscard]] Expected<Growth> prepareGrowth(std::uint32_t capacity) noexcept
        {
            return storage_.prepareGrowth(capacity);
        }

        void commitGrowth(Growth growth) noexcept
        {
            storage_.commitGrowth(std::move(growth));
        }

        [[nodiscard]] VkDeviceAddress pageAddress(const Growth& growth, std::uint32_t index) const noexcept
        {
            return storage_.pageAddress(growth, index);
        }

        explicit TSparseInstanceStream(SparseInstanceStreamStorage storage) noexcept : storage_(std::move(storage)) {}

        SparseInstanceStreamStorage storage_;
    };

    template <class T> class TStableInstanceCpuPages final
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
