#pragma once

#include <lux/engine/render/gpu/memory/PagedGpuStream.hpp>
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

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

        /// 注册(或按内容复用)一条段记录。
        ///
        /// @param ibo_segment 该网格索引数据所在的 ChainedArena 段号。**只参与 CPU 侧
        ///        去重键,不上传 GPU**(MeshSectionRecord 是 16 字节定长,布局与两个
        ///        计算着色器的 std430 声明绑死,不能加字段)。之所以必须入键:
        ///        MeshSectionRecord::first_index 是段内相对值,段 0 与段 1 里偏移相同、
        ///        面数相同的两个不同网格会算出完全一致的四元组,不带段号就会被去重成
        ///        同一个 section id —— 在跨段几何错误之上再叠一层别名。
        ///        当前 GPU 驱动路径只接受段 0 的网格(serverAddMeshInstance 处强制),
        ///        所以实际取值恒为 0;此参数是给将来的按段分桶留的正确性余量。
        [[nodiscard]] uint32_t registerSection(
            const MeshSectionRecord& section,
            uint16_t ibo_segment = 0,
            VkIndexType index_type = VK_INDEX_TYPE_UINT32
        );
        void unregisterSection(uint32_t section_id);

        [[nodiscard]] const MeshSectionRecord& at(uint32_t section_id) const noexcept;
        [[nodiscard]] VkBuffer buffer() const noexcept
        {
            return stream_.buffer();
        }
        [[nodiscard]] bool hasWork() const noexcept
        {
            return !cpu_only_mode_ && (full_rebuild_ || stream_.hasDirtyPages());
        }
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            stream_.setDeferredQueue(q);
        }

        bool ensureCapacity(uint32_t required);

        /// Transfer subsystem path — submit copy requests to scheduler.
        void submitTransfers(TransferScheduler& scheduler);

        void markFullRebuild() noexcept
        {
            full_rebuild_ = true;
        }

    private:
        struct SectionKey
        {
            uint32_t first_index{0};
            uint32_t index_count{0};
            int32_t base_vertex{0};
            uint32_t vertex_count{0};
            /// 段号 —— first_index 是段内相对值,不入键会让不同段的网格互相别名。
            uint16_t ibo_segment{0};
            VkIndexType index_type{VK_INDEX_TYPE_UINT32};

            [[nodiscard]] bool operator==(const SectionKey& rhs) const noexcept
            {
                return first_index == rhs.first_index && index_count == rhs.index_count &&
                       base_vertex == rhs.base_vertex && vertex_count == rhs.vertex_count &&
                       ibo_segment == rhs.ibo_segment && index_type == rhs.index_type;
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
                h ^= static_cast<size_t>(key.ibo_segment) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= static_cast<size_t>(key.index_type) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        [[nodiscard]] static SectionKey
        makeSectionKey(const MeshSectionRecord& section, uint16_t ibo_segment, VkIndexType index_type) noexcept;

        PagedGpuStream<MeshSectionRecord> stream_;
        // Upload-chunk scratch reused across ticks (cleared at submitTransfers
        // entry; collectUploadChunks only appends). Single set is correct —
        // produced + consumed synchronously within one render-thread tick. (P-5)
        std::vector<PagedGpuStream<MeshSectionRecord>::UploadChunk> chunks_;
        std::vector<uint8_t> alive_;
        /// 每条 section 的 ibo_segment,与 alive_/ref_counts_ 同下标。GPU 记录里放不下
        /// (MeshSectionRecord 定长 16 字节),但 unregisterSection 要靠它重建去重键。
        std::vector<uint16_t> segments_;
        std::vector<VkIndexType> index_types_;
        std::vector<uint32_t> ref_counts_;
        std::vector<uint32_t> free_ids_;
        std::unordered_map<SectionKey, uint32_t, SectionKeyHash> dedup_map_;
        uint32_t count_{0};
        bool full_rebuild_{true};
        bool cpu_only_mode_{false};
        std::vector<MeshSectionRecord> cpu_sections_;
    };

} // namespace lux::render
