#include <lux/engine/render/resources/mesh/MeshSectionTable.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace lux::render
{

    MeshSectionTable::SectionKey MeshSectionTable::makeSectionKey(
        const MeshSectionRecord& section,
        uint16_t ibo_segment,
        VkIndexType index_type
    ) noexcept
    {
        SectionKey key{};
        key.first_index = section.first_index;
        key.index_count = section.index_count;
        key.base_vertex = section.base_vertex;
        key.vertex_count = section.vertex_count;
        key.ibo_segment = ibo_segment;
        key.index_type = index_type;
        return key;
    }

    void MeshSectionTable::init(DeviceContext* device_context, uint32_t initial_capacity)
    {
        cpu_only_mode_ = (device_context == nullptr);
        if (!cpu_only_mode_)
            stream_.init(device_context, initial_capacity);
        else
            cpu_sections_.assign(initial_capacity, MeshSectionRecord{});

        alive_.assign(initial_capacity, 0u);
        segments_.assign(initial_capacity, 0u);
        index_types_.assign(initial_capacity, VK_INDEX_TYPE_UINT32);
        ref_counts_.assign(initial_capacity, 0u);
        free_ids_.clear();
        dedup_map_.clear();
        dedup_map_.reserve(initial_capacity);
        count_ = 0u;
        full_rebuild_ = !cpu_only_mode_;
    }

    void MeshSectionTable::shutdown()
    {
        if (!cpu_only_mode_)
            stream_.shutdown();
        cpu_sections_.clear();
        alive_.clear();
        segments_.clear();
        index_types_.clear();
        ref_counts_.clear();
        free_ids_.clear();
        dedup_map_.clear();
        count_ = 0u;
        full_rebuild_ = true;
        cpu_only_mode_ = false;
    }

    bool MeshSectionTable::ensureCapacity(uint32_t required)
    {
        const uint32_t current_capacity =
            cpu_only_mode_ ? static_cast<uint32_t>(cpu_sections_.size()) : stream_.capacity();

        if (required <= current_capacity)
            return true;

        if (!cpu_only_mode_ && !stream_.reserve(required))
            return false;

        if (cpu_only_mode_)
            cpu_sections_.resize(required, MeshSectionRecord{});

        alive_.resize(required, 0u);
        segments_.resize(required, 0u);
        index_types_.resize(required, VK_INDEX_TYPE_UINT32);
        ref_counts_.resize(required, 0u);
        return true;
    }

    uint32_t
    MeshSectionTable::registerSection(const MeshSectionRecord& section, uint16_t ibo_segment, VkIndexType index_type)
    {
        const SectionKey key = makeSectionKey(section, ibo_segment, index_type);
        const auto dedup_it = dedup_map_.find(key);
        if (dedup_it != dedup_map_.end())
        {
            const uint32_t id = dedup_it->second;
            if (id < alive_.size() && alive_[id] != 0u)
            {
                if (id >= ref_counts_.size())
                    ref_counts_.resize(id + 1u, 0u);

                if (ref_counts_[id] == std::numeric_limits<uint32_t>::max())
                    return kInvalidSectionId;

                ++ref_counts_[id];
                return id;
            }

            // Recover from stale mapping if an id was retired unexpectedly.
            dedup_map_.erase(dedup_it);
        }

        uint32_t id = kInvalidSectionId;
        if (!free_ids_.empty())
        {
            id = free_ids_.back();
            free_ids_.pop_back();

            if (id >= alive_.size() && !ensureCapacity(id + 1u))
                return kInvalidSectionId;
            if (id >= count_)
                count_ = id + 1u;
        }
        else
        {
            id = count_;
            if (!ensureCapacity(id + 1u))
                return kInvalidSectionId;
            ++count_;
        }

        alive_[id] = 1u;
        if (id >= ref_counts_.size())
            ref_counts_.resize(id + 1u, 0u);
        ref_counts_[id] = 1u;
        if (id >= segments_.size())
            segments_.resize(id + 1u, 0u);
        segments_[id] = ibo_segment;
        if (id >= index_types_.size())
            index_types_.resize(id + 1u, VK_INDEX_TYPE_UINT32);
        index_types_[id] = index_type;
        if (cpu_only_mode_)
        {
            cpu_sections_[id] = section;
        }
        else
        {
            stream_.at(id) = section;
            stream_.markDirty(id);
        }
        dedup_map_.insert_or_assign(key, id);
        return id;
    }

    void MeshSectionTable::unregisterSection(uint32_t section_id)
    {
        if (section_id >= alive_.size() || alive_[section_id] == 0u)
            return;

        if (section_id >= ref_counts_.size() || ref_counts_[section_id] == 0u)
            return;

        const uint32_t remaining_refs = --ref_counts_[section_id];
        if (remaining_refs > 0u)
            return;

        const MeshSectionRecord& section = cpu_only_mode_ ? cpu_sections_[section_id] : stream_.at(section_id);
        const uint16_t seg = section_id < segments_.size() ? segments_[section_id] : 0u;
        const VkIndexType index_type =
            section_id < index_types_.size() ? index_types_[section_id] : VK_INDEX_TYPE_UINT32;
        const SectionKey key = makeSectionKey(section, seg, index_type);
        const auto dedup_it = dedup_map_.find(key);
        if (dedup_it != dedup_map_.end() && dedup_it->second == section_id)
            dedup_map_.erase(dedup_it);

        alive_[section_id] = 0u;
        ref_counts_[section_id] = 0u;
        if (section_id < segments_.size())
            segments_[section_id] = 0u;
        if (section_id < index_types_.size())
            index_types_[section_id] = VK_INDEX_TYPE_UINT32;
        free_ids_.push_back(section_id);

        // Keep a safe zeroed section for stale GPU references.
        if (cpu_only_mode_)
        {
            cpu_sections_[section_id] = MeshSectionRecord{};
        }
        else
        {
            stream_.at(section_id) = MeshSectionRecord{};
            stream_.markDirty(section_id);
        }

        // Keep count_ as a dense high-watermark to avoid uploading dead tail slots.
        if (section_id + 1u == count_)
        {
            while (count_ > 0u && alive_[count_ - 1u] == 0u)
                --count_;

            if (!free_ids_.empty())
            {
                free_ids_.erase(
                    std::remove_if(free_ids_.begin(), free_ids_.end(), [this](uint32_t id) { return id >= count_; }),
                    free_ids_.end()
                );
            }
        }
    }

    const MeshSectionRecord& MeshSectionTable::at(uint32_t section_id) const noexcept
    {
        static const MeshSectionRecord kNullSection{};
        if (section_id >= alive_.size() || alive_[section_id] == 0u)
            return kNullSection;

        if (cpu_only_mode_)
            return cpu_sections_[section_id];

        return stream_.at(section_id);
    }

    void MeshSectionTable::submitTransfers(TransferScheduler& scheduler)
    {
        if (cpu_only_mode_ || count_ == 0u)
            return;

        if (!full_rebuild_ && !stream_.hasDirtyPages())
            return;

        chunks_.clear();
        const VkDeviceSize total_bytes = stream_.collectUploadChunks(count_, full_rebuild_, chunks_);
        if (total_bytes == 0u)
            return;

        auto stg = scheduler.allocateStaging(total_bytes);
        if (!stg)
            return;

        auto* dst = static_cast<uint8_t*>(stg.mapped);
        VkDeviceSize offset = 0;
        for (const auto& chunk : chunks_)
        {
            std::memcpy(dst + offset, chunk.src, static_cast<size_t>(chunk.size));
            scheduler.submitBufferCopy({
                .src = stg.buffer,
                .src_offset = stg.srcOffset + offset,
                .dst = stream_.buffer(),
                .dst_offset = chunk.dst_offset,
                .size = chunk.size,
                .domain = EBufferDomain::Storage_CS,
            }
            );
            offset += chunk.size;
        }

        // Reached only on a successful staging alloc + copy emission (early returns
        // above cover total==0 and staging failure). Clear the page-dirty state so
        // already-uploaded pages do not re-upload every subsequent frame — without
        // this the incremental page-dirty design is defeated (pages stay dirty
        // forever -> full re-upload each frame).
        stream_.clearDirtyState();
        if (full_rebuild_)
            full_rebuild_ = false;
    }

} // namespace lux::render
