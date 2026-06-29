#pragma once
/**
 * @file MdcTable.hpp
 * @brief Mesh Draw Command (MDC) table for GPU-driven instanced rendering.
 *
 * An MDC is a unique combination of (geometry_kind, bucket_id, section_id).
 * Each MDC maps to exactly one VkDrawIndexedIndirectCommand at draw time.
 * Multiple instances sharing the same MDC are batched via instanceCount.
 *
 * The table assigns a dense mdc_index to each unique key.  GPU buffers are
 * addressed using per-MDC offsets computed by buildOffsets().
 */

#include <cstdint>
#include <vector>
#include <unordered_map>

namespace lux::render
{
    // =========================================================================
    //  MdcKey — unique draw-command identity
    // =========================================================================
    struct MdcKey
    {
        uint8_t geometry_kind;
        uint32_t bucket_id;
        uint32_t section_id;

        bool operator==(const MdcKey &) const noexcept = default;
    };

    struct MdcKeyHash
    {
        size_t operator()(const MdcKey &k) const noexcept
        {
            size_t h = std::hash<uint32_t>{}(k.section_id);
            h ^= std::hash<uint32_t>{}(k.bucket_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint8_t>{}(k.geometry_kind) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // =========================================================================
    //  MdcEntry — per-MDC metadata (CPU side)
    // =========================================================================
    struct MdcEntry
    {
        uint32_t section_id;
        uint8_t geometry_kind;
        uint32_t bucket_id;
        uint32_t instance_count; ///< live instances referencing this MDC
        uint32_t visible_offset; ///< base offset in visible buffer (set by buildOffsets)
        // Sticky high-water visible-buffer capacity (power-of-two >= instance_count).
        // buildOffsets reserves this many slots per MDC instead of the exact live
        // count, so the per-MDC offsets — and the total visible-buffer size — stay
        // STABLE while instance_count varies within the same power-of-two band.
        // Only a band crossing (or a bucket appearing/dying) shifts offsets and
        // bumps layout_serial_, which is what triggers a graph recompile. This is
        // what lets spawn/despawn of existing meshes avoid a full recompile. Reset
        // to 0 when the slot dies (so a dead slot reserves nothing). (P-7)
        uint32_t capacity;
    };

    // =========================================================================
    //  MdcTable
    // =========================================================================
    class MdcTable
    {
    public:
        /// Register an instance with the given key.
        /// Returns the MDC index for storage in InstanceCullMeta.mdc_index.
        /// If the key already exists, increments instance_count and returns the existing index.
        uint32_t registerInstance(uint8_t geometry_kind, uint32_t bucket_id, uint32_t section_id);

        /// Unregister an instance.  Decrements instance_count for the given MDC.
        void unregisterInstance(uint32_t mdc_index);

        /// Rebuild per-MDC visible-buffer offsets from current instance counts.
        /// Must be called once per frame before GPU upload.
        void buildOffsets();

        /// Number of MDC entries.
        [[nodiscard]] uint32_t count() const noexcept
        {
            return static_cast<uint32_t>(entries_.size());
        }

        /// Read-only access to entries.
        [[nodiscard]] const std::vector<MdcEntry> &entries() const noexcept { return entries_; }

        /// GPU-uploadable packed data.
        /// Layout: mdc_count × {uint32_t offset, uint32_t section_id},
        ///         plus one sentinel {total_capacity, 0}.
        /// Total element count = (mdc_count + 1) * 2.
        [[nodiscard]] const std::vector<uint32_t> &gpuData() const noexcept { return gpu_data_; }

        /// Size of the GPU data buffer in bytes.
        [[nodiscard]] uint32_t gpuDataSizeBytes() const noexcept
        {
            return static_cast<uint32_t>(gpu_data_.size()) * sizeof(uint32_t);
        }

        /// Total visible-buffer capacity across all MDCs (in uint32_t entries).
        [[nodiscard]] uint32_t totalVisibleCapacity() const noexcept { return total_visible_capacity_; }

        /// Live sum of instance_count across all MDCs (no buildOffsets needed).
        [[nodiscard]] uint32_t liveInstanceTotal() const noexcept
        {
            uint32_t total = 0;
            for (const auto &e : entries_)
                total += e.instance_count;
            return total;
        }

        /// Monotonic counter bumped on EVERY register/unregister/clear — the exact
        /// per-mutation fingerprint. Currently has no consumers.
        /// WARNING: do NOT key a graph recompile on this — it bumps on every single
        /// instance add/remove, which is exactly the per-frame full-recompile that
        /// P-7 eliminated. Use layoutSerial() (below) as the recompile trigger; it
        /// only bumps when the per-MDC offset LAYOUT actually changes. mutationSerial
        /// is kept only as an exact change fingerprint for a future consumer that
        /// genuinely needs per-mutation granularity (none today).
        [[nodiscard]] uint64_t mutationSerial() const noexcept { return mutation_serial_; }

        /// Coarser fingerprint than mutationSerial: bumped ONLY when the per-MDC
        /// offset LAYOUT changes — a capacity band crossing, a new bucket, a bucket
        /// dying, or clear(). Adding/removing an instance within an existing
        /// bucket's current power-of-two capacity band does NOT bump it (offsets and
        /// the visible-buffer size are unchanged). The GPU-driven mesh features use
        /// THIS (not mutationSerial) as the recompile trigger so steady-state
        /// spawn/despawn of existing meshes no longer forces a full graph recompile
        /// every frame. mutationSerial stays the exact per-mutation fingerprint for
        /// consumers that need it. (P-7)
        [[nodiscard]] uint64_t layoutSerial() const noexcept { return layout_serial_; }

        /// Power-of-two visible-buffer capacity band for @p n live instances:
        /// 0 -> 0, else the smallest power of two >= n. Used by buildOffsets and by
        /// the shadow feature's per-frame offset build so both reserve the SAME
        /// per-MDC capacity (keeping their offsets in lock-step). (P-7)
        [[nodiscard]] static uint32_t bucketCapacity(uint32_t n) noexcept
        {
            if (n <= 1u) return n;            // 0 -> 0, 1 -> 1
            --n;
            n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16;
            return n + 1u;
        }

        /// Reset the table (used on scene teardown).
        void clear();

    private:
        std::unordered_map<MdcKey, uint32_t, MdcKeyHash> key_to_index_;
        std::vector<MdcEntry> entries_;
        std::vector<uint32_t> free_list_;  ///< dead entry slots available for reuse (M10)

        // GPU-uploadable arrays (built by buildOffsets)
        std::vector<uint32_t> gpu_data_; ///< interleaved {offset, section_id}
        uint32_t total_visible_capacity_{0};
        uint64_t mutation_serial_{0};    ///< bumped on every count-affecting mutation
        uint64_t layout_serial_{0};      ///< bumped only on offset-LAYOUT changes (P-7)
    };

} // namespace lux::render
