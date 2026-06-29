#pragma once

#include <lux/engine/render/resources/memory/GPUBuffer.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <vector>

namespace lux::render
{

    template <typename T>
    class PagedGpuStream
    {
    public:
        static constexpr uint32_t kUploadPageSize = 512u;

        struct UploadChunk
        {
            const uint8_t* src{nullptr};
            VkDeviceSize dst_offset{0};
            VkDeviceSize size{0};
        };

        void init(DeviceContext* device_context, uint32_t capacity,
                  VkBufferUsageFlags extra_usage = 0)
        {
            GpuBufferCreateInfo ci{};
            ci.device_context = device_context;
            ci.initial_capacity = capacity;
            ci.buffer_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                | extra_usage;
            gpu_buf_.init(ci);

            cpu_data_.resize(capacity);
            page_flags_.assign(pageCountForCapacity(capacity), 0u);
        }

        void shutdown()
        {
            gpu_buf_.destroy();
            cpu_data_.clear();
            dirty_pages_.clear();
            page_flags_.clear();
        }

        [[nodiscard]] T& at(uint32_t index) noexcept { return cpu_data_[index]; }
        [[nodiscard]] const T& at(uint32_t index) const noexcept { return cpu_data_[index]; }

        [[nodiscard]] std::vector<T>& cpuData() noexcept { return cpu_data_; }
        [[nodiscard]] const std::vector<T>& cpuData() const noexcept { return cpu_data_; }

        void markDirty(uint32_t index)
        {
            const uint32_t page = pageIndexForSlot(index);
            auto& flag = page_flags_[page];
            if (flag == 0u)
            {
                flag = 1u;
                dirty_pages_.push_back(page);
            }
        }

        [[nodiscard]] bool hasDirtyPages() const noexcept { return !dirty_pages_.empty(); }

        [[nodiscard]] bool reserve(uint32_t new_capacity)
        {
            if (new_capacity <= gpu_buf_.capacity())
                return true;
            if (!gpu_buf_.reserve(new_capacity, false))
                return false;

            cpu_data_.resize(new_capacity);
            page_flags_.resize(pageCountForCapacity(new_capacity), 0u);
            return true;
        }

        void compact(const std::vector<uint32_t>& old_to_new_remap, uint32_t live_count)
        {
            if (live_count == 0u)
            {
                clearDirtyState();
                return;
            }

            std::vector<T> compact_data(live_count);
            for (uint32_t old_slot = 0; old_slot < old_to_new_remap.size(); ++old_slot)
            {
                const uint32_t new_slot = old_to_new_remap[old_slot];
                if (new_slot == ~0u || new_slot >= live_count)
                    continue;
                compact_data[new_slot] = cpu_data_[old_slot];
            }

            for (uint32_t slot = 0; slot < live_count; ++slot)
                cpu_data_[slot] = compact_data[slot];

            clearDirtyState();
        }

        [[nodiscard]] VkDeviceSize collectUploadChunks(
            uint32_t count,
            bool full_upload,
            std::vector<UploadChunk>& chunks)
        {
            const auto* cpu_bytes = reinterpret_cast<const uint8_t*>(cpu_data_.data());
            const VkDeviceSize element_stride = static_cast<VkDeviceSize>(sizeof(T));

            if (full_upload)
            {
                const VkDeviceSize byte_count = static_cast<VkDeviceSize>(count) * element_stride;
                if (byte_count == 0u)
                {
                    clearDirtyState();
                    return 0u;
                }

                chunks.push_back(UploadChunk{
                    .src = cpu_bytes,
                    .dst_offset = 0u,
                    .size = byte_count,
                });
                return byte_count;
            }

            if (dirty_pages_.empty())
                return 0u;

            std::ranges::sort(dirty_pages_);

            // Reuse the member scratch (cleared per call) instead of a fresh local
            // vector — collectUploadChunks runs every frame any stream is dirty. (P-5)
            runs_scratch_.clear();
            runs_scratch_.reserve(dirty_pages_.size());

            uint32_t i = 0u;
            while (i < dirty_pages_.size())
            {
                const uint32_t first_page = dirty_pages_[i];
                uint32_t last_page = first_page;
                ++i;
                while (i < dirty_pages_.size() && dirty_pages_[i] == (last_page + 1u))
                {
                    ++last_page;
                    ++i;
                }

                const uint32_t first_slot = first_page * kUploadPageSize;
                uint32_t end_slot = (last_page + 1u) * kUploadPageSize;
                end_slot = std::min(end_slot, count);
                if (first_slot >= end_slot)
                    continue;
                runs_scratch_.push_back({first_slot, end_slot - first_slot});
            }

            VkDeviceSize total_bytes = 0u;
            chunks.reserve(chunks.size() + runs_scratch_.size());
            for (const auto& run : runs_scratch_)
            {
                const size_t byte_offset = static_cast<size_t>(run.first_slot) * sizeof(T);
                const VkDeviceSize bytes = static_cast<VkDeviceSize>(run.slot_count) * element_stride;
                chunks.push_back(UploadChunk{
                    .src = cpu_bytes + byte_offset,
                    .dst_offset = static_cast<VkDeviceSize>(byte_offset),
                    .size = bytes,
                });
                total_bytes += bytes;
            }

            if (total_bytes == 0u)
                clearDirtyState();

            return total_bytes;
        }

        void clearDirtyState()
        {
            for (const uint32_t page : dirty_pages_)
                page_flags_[page] = 0u;
            dirty_pages_.clear();
        }

        [[nodiscard]] VkBuffer buffer() const noexcept { return gpu_buf_.buffer(); }
        [[nodiscard]] uint32_t capacity() const noexcept { return gpu_buf_.capacity(); }

        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            gpu_buf_.setDeferredQueue(q);
        }

    private:
        static constexpr uint32_t pageCountForCapacity(uint32_t capacity) noexcept
        {
            return (capacity + kUploadPageSize - 1u) / kUploadPageSize;
        }

        static constexpr uint32_t pageIndexForSlot(uint32_t slot) noexcept
        {
            return slot / kUploadPageSize;
        }

        // Coalesced dirty-page run; reused across frames (cleared per call) so the
        // incremental collectUploadChunks path allocates nothing on the upload
        // hot path. (P-5)
        struct Run
        {
            uint32_t first_slot;
            uint32_t slot_count;
        };

        std::vector<T> cpu_data_;
        GpuBuffer<T, EGpuBufferType::GPU_ONLY, false> gpu_buf_;
        std::vector<uint32_t> dirty_pages_;
        std::vector<uint8_t> page_flags_;
        std::vector<Run> runs_scratch_;  ///< per-call reused; see Run above
    };

} // namespace lux::render

