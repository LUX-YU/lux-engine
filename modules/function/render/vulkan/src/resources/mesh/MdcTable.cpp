#include <lux/engine/render/resources/mesh/MdcTable.hpp>
#include <cassert>
#include <numeric>

namespace lux::render
{
    uint32_t MdcTable::registerInstance(
        uint8_t geometry_kind,
        uint32_t bucket_id,
        uint32_t section_id,
        uint16_t ibo_segment,
        VkIndexType index_type
    )
    {
        MdcKey key{geometry_kind, bucket_id, section_id, ibo_segment, index_type};
        auto it = key_to_index_.find(key);
        if (it != key_to_index_.end())
        {
            MdcEntry& entry = entries_[it->second];
            entry.instance_count += 1;
            ++mutation_serial_; // exact per-mutation fingerprint
            // Only a power-of-two capacity band crossing shifts later offsets and
            // thus needs a recompile; staying within the band is free. (P-7)
            if (entry.instance_count > entry.capacity)
            {
                entry.capacity = bucketCapacity(entry.instance_count);
                ++layout_serial_;
            }
            return it->second;
        }

        // New key: reuse a recycled (dead) slot if one is free, else append. Reuse
        // bounds entries_ to the high-water mark of *concurrently* live MDCs rather
        // than the total ever created — the editor material-recompile flow produces
        // a fresh bucket_id (=> a new MDC key) on every edit, so without recycling
        // the table, its GPU buffers and the per-MDC dispatch count grew
        // monotonically for the scene's whole lifetime. (M10)
        uint32_t index;
        if (!free_list_.empty())
        {
            index = free_list_.back();
            free_list_.pop_back();
            MdcEntry& entry = entries_[index];
            entry.section_id = section_id;
            entry.geometry_kind = geometry_kind;
            entry.bucket_id = bucket_id;
            entry.ibo_segment = ibo_segment;
            entry.index_type = index_type;
            entry.instance_count = 1;
            entry.visible_offset = 0;
            entry.capacity = bucketCapacity(1); // dead slot had capacity 0
        }
        else
        {
            index = static_cast<uint32_t>(entries_.size());
            MdcEntry entry{};
            entry.section_id = section_id;
            entry.geometry_kind = geometry_kind;
            entry.bucket_id = bucket_id;
            entry.ibo_segment = ibo_segment;
            entry.index_type = index_type;
            entry.instance_count = 1;
            entry.visible_offset = 0;
            entry.capacity = bucketCapacity(1);
            entries_.push_back(entry);
        }

        key_to_index_.emplace(key, index);
        ++mutation_serial_; // new MDC -> new offset layout
        ++layout_serial_;   // a new/revived bucket shifts the offset layout (P-7)
        return index;
    }

    void MdcTable::unregisterInstance(uint32_t mdc_index)
    {
        if (mdc_index >= entries_.size())
            return;
        auto& e = entries_[mdc_index];
        if (e.instance_count > 0)
        {
            --e.instance_count;
            ++mutation_serial_; // exact per-mutation fingerprint
            // Capacity is sticky: a count drop within a live bucket keeps the band
            // (offsets unchanged) so no recompile — only bucket DEATH below shifts
            // the layout. (P-7)

            if (e.instance_count == 0)
            {
                e.capacity = 0;   // dead slot reserves no visible-buffer space
                ++layout_serial_; // bucket death shifts every later MDC's offset
                // Slot is now dead. Drop its key and recycle the slot so a future
                // registerInstance can reuse it. Index stability is preserved: a
                // slot only dies once NO live instance references it, so no caller
                // can still be holding this mdc_index. This also fixes the
                // "zeroed entry silently revives" wrinkle — a later instance with
                // the same key now takes the normal new-key path instead of
                // resurrecting a stale entry. (M10)
                key_to_index_.erase(MdcKey{e.geometry_kind, e.bucket_id, e.section_id, e.ibo_segment, e.index_type});
                free_list_.push_back(mdc_index);
            }
        }
    }

    void MdcTable::buildOffsets()
    {
        const uint32_t n = count();
        // GPU data: (n + 1) pairs of {offset, section_id} + safety padding.
        // When n == 0 we still need 4 uint32s so that the shader can safely
        // read data[0] and data[2] without going out-of-bounds.
        const size_t min_pairs = std::max<size_t>(n + 1, 2);
        gpu_data_.resize(min_pairs * 2, 0u);

        uint32_t running_offset = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            const auto& e = entries_[i];
            gpu_data_[i * 2 + 0] = running_offset;
            gpu_data_[i * 2 + 1] = e.section_id;
            entries_[i].visible_offset = running_offset;
            // Reserve the power-of-two band, not the exact live count, so offsets
            // are stable across same-band count changes (a dead slot has capacity 0
            // and reserves nothing). The cull/compact already read per-MDC capacity
            // as the gap between consecutive offsets, so the extra headroom is
            // simply never written/read. (P-7)
            running_offset += e.capacity;
        }
        // Sentinel
        gpu_data_[n * 2 + 0] = running_offset;
        gpu_data_[n * 2 + 1] = 0;

        total_visible_capacity_ = running_offset;
    }

    void MdcTable::clear()
    {
        key_to_index_.clear();
        entries_.clear();
        free_list_.clear();
        gpu_data_.clear();
        total_visible_capacity_ = 0;
        ++mutation_serial_; // monotonic: never reset, so a reused table still invalidates
        ++layout_serial_;   // the offset layout is gone too (P-7)
    }

} // namespace lux::render
