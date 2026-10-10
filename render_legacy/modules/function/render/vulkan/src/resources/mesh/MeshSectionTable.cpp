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

    Expected<MeshSectionTable>
    MeshSectionTable::create(DeviceContext& device, DeferredDestroyQueue& retirement, uint32_t initial_capacity) noexcept
    {
        auto stream = Stream::create(device, retirement, initial_capacity);
        if (!stream)
        {
            return lux::cxx::unexpected(stream.error());
        }
        return MeshSectionTable{VStorage{std::move(*stream)}, initial_capacity};
    }

    MeshSectionTable MeshSectionTable::createCpu(uint32_t initial_capacity) noexcept
    {
        return MeshSectionTable{VStorage{std::vector<MeshSectionRecord>(initial_capacity)}, initial_capacity};
    }

    MeshSectionTable::MeshSectionTable(VStorage storage, uint32_t capacity) noexcept
        : storage_(std::move(storage)), alive_(capacity), segments_(capacity),
          index_types_(capacity, VK_INDEX_TYPE_UINT32), ref_counts_(capacity)
    {
        dedup_map_.reserve(capacity);
    }

    Expected<void> MeshSectionTable::ensureCapacity(uint32_t required) noexcept
    {
        if (required <= alive_.size())
        {
            return {};
        }
        if (auto* stream = std::get_if<Stream>(&storage_))
        {
            const auto previous = stream->buffer();
            auto resized = stream->reserve(required);
            if (!resized)
            {
                return resized;
            }
            full_rebuild_ = full_rebuild_ || previous != stream->buffer();
        }
        else
        {
            std::get<std::vector<MeshSectionRecord>>(storage_).resize(required);
        }
        alive_.resize(required, 0u);
        segments_.resize(required, 0u);
        index_types_.resize(required, VK_INDEX_TYPE_UINT32);
        ref_counts_.resize(required, 0u);
        return {};
    }

    void MeshSectionTable::writeRecord(uint32_t id, const MeshSectionRecord& value) noexcept
    {
        if (auto* stream = std::get_if<Stream>(&storage_))
        {
            stream->at(id) = value;
            stream->markDirty(id);
        }
        else
        {
            std::get<std::vector<MeshSectionRecord>>(storage_)[id] = value;
        }
    }

    const MeshSectionRecord& MeshSectionTable::record(uint32_t id) const noexcept
    {
        if (const auto* stream = std::get_if<Stream>(&storage_))
        {
            return stream->at(id);
        }
        return std::get<std::vector<MeshSectionRecord>>(storage_)[id];
    }

    uint32_t MeshSectionTable::registerSection(
        const MeshSectionRecord& section,
        uint16_t ibo_segment,
        VkIndexType index_type
    )
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
        writeRecord(id, section);
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

        const MeshSectionRecord& section = record(section_id);
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
        writeRecord(section_id, MeshSectionRecord{});

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

        return record(section_id);
    }

    void MeshSectionTable::submitTransfers(TransferScheduler& scheduler)
    {
        auto* stream = std::get_if<Stream>(&storage_);
        const bool has_no_upload = !stream || count_ == 0u;
        if (has_no_upload)
        {
            return;
        }

        if (!full_rebuild_ && !stream->hasDirtyPages())
        {
            return;
        }

        chunks_.clear();
        const VkDeviceSize total_bytes = stream->collectUploadChunks(count_, full_rebuild_, chunks_);
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
                .dst = stream->buffer(),
                .dst_offset = chunk.dst_offset,
                .size = chunk.size,
                .domain = EBufferDomain::STORAGE_CS,
            });
            offset += chunk.size;
        }

        // Reached only on a successful staging alloc + copy emission (early returns
        // above cover total==0 and staging failure). Clear the page-dirty state so
        // already-uploaded pages do not re-upload every subsequent frame — without
        // this the incremental page-dirty design is defeated (pages stay dirty
        // forever -> full re-upload each frame).
        stream->clearDirtyState();
        if (full_rebuild_)
            full_rebuild_ = false;
    }

} // namespace lux::render
