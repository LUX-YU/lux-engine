#pragma once

#include <lux/engine/render/resources/memory/PagedGpuStream.hpp>
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lux::render
{
    class TransferScheduler;

    struct alignas(16) MeshSectionRecord
    {
        uint32_t first_index{0};
        uint32_t index_count{0};
        int32_t base_vertex{0};
        uint32_t vertex_count{0};
    };
    static_assert(sizeof(MeshSectionRecord) == 16);

    class LUX_FUNCTION_PUBLIC MeshSectionTable
    {
    public:
        static constexpr uint32_t kInvalidSectionId = ~0u;

        void init(DeviceContext* device_context, uint32_t initial_capacity);
        void shutdown();

        [[nodiscard]] uint32_t registerSection(const MeshSectionRecord& section);
        void unregisterSection(uint32_t section_id);

        [[nodiscard]] const MeshSectionRecord& at(uint32_t section_id) const noexcept;
        [[nodiscard]] VkBuffer buffer() const noexcept { return stream_.buffer(); }
        [[nodiscard]] bool hasWork() const noexcept { return !cpu_only_mode_ && (full_rebuild_ || stream_.hasDirtyPages()); }
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept { stream_.setDeferredQueue(q); }

        bool ensureCapacity(uint32_t required);

        /// Transfer subsystem path — submit copy requests to scheduler.
        void submitTransfers(TransferScheduler& scheduler);

        void markFullRebuild() noexcept { full_rebuild_ = true; }

    private:
        struct SectionKey
        {
            uint32_t first_index{0};
            uint32_t index_count{0};
            int32_t base_vertex{0};
            uint32_t vertex_count{0};

            [[nodiscard]] bool operator==(const SectionKey& rhs) const noexcept
            {
                return first_index == rhs.first_index
                    && index_count == rhs.index_count
                    && base_vertex == rhs.base_vertex
                    && vertex_count == rhs.vertex_count;
            }
        };

        struct SectionKeyHash
        {
            [[nodiscard]] size_t operator()(const SectionKey& key) const noexcept
            {
                size_t h = static_cast<size_t>(key.first_index);
                h ^= static_cast<size_t>(key.index_count) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= static_cast<size_t>(static_cast<uint32_t>(key.base_vertex)) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= static_cast<size_t>(key.vertex_count) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        [[nodiscard]] static SectionKey makeSectionKey(const MeshSectionRecord& section) noexcept;

        PagedGpuStream<MeshSectionRecord> stream_;
        // Upload-chunk scratch reused across ticks (cleared at submitTransfers
        // entry; collectUploadChunks only appends). Single set is correct —
        // produced + consumed synchronously within one render-thread tick. (P-5)
        std::vector<PagedGpuStream<MeshSectionRecord>::UploadChunk> chunks_;
        std::vector<uint8_t> alive_;
        std::vector<uint32_t> ref_counts_;
        std::vector<uint32_t> free_ids_;
        std::unordered_map<SectionKey, uint32_t, SectionKeyHash> dedup_map_;
        uint32_t count_{0};
        bool full_rebuild_{true};
        bool cpu_only_mode_{false};
        std::vector<MeshSectionRecord> cpu_sections_;
    };

} // namespace lux::render
