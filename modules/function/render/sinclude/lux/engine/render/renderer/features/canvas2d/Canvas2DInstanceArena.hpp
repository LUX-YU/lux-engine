#pragma once
// ============================================================================
//  Canvas2DInstanceArena.hpp — the scene's multi-KIND GPU-resident 2D instance
//  store (Canvas2D v2 + C2D-R6a; .internal/2d-gpu-driven-rewrite.md §3.1/§3.7).
//
//  ONE shared ordering axis, N instance KINDS (sprite today; pixel-field chunk,
//  tile chunk, particle emitter arrive with their slices). Every live+visible
//  instance owns a key in a single strict-total-order space:
//
//      key = (quantizePriority(priority) << 32) | (kind << 28) | slot
//
//  so ANY kind can sit at ANY user-set depth, freely interleaved (2026-07-06
//  ruling). Equal priorities tie-break by kind id then creation order —
//  deterministic, and it clusters same-kind instances into minimal runs.
//
//  Each KIND owns: its record type + PagedGpuStream, generational slots, its
//  ORDER stream (the kind's subsequence of the global ascending order) and its
//  descriptor set (binding 0 = records, binding 1 = order — one shared layout,
//  every kind has the same 2×storage shape). SHARED: the key space, the order
//  rebuild (still only on key-change frames), the run table and the enable bit.
//
//  The order rebuild emits (a) each kind's order subsequence and (b) a CPU RUN
//  table — consecutive same-kind segments {kind, first, count}, where `first`
//  is the offset inside that kind's order buffer. The draw kernel walks runs;
//  `vkCmdDraw*(…, firstInstance = first)` addresses the subsequence directly
//  (gl_InstanceIndex includes firstInstance), so kind vertex shaders never
//  change. Pipeline/DS switching between runs lands with the second kind
//  (C2D-R6b, F2-09) — a single-kind scene is ONE run, exactly the pre-R6
//  behaviour (zero regression by construction).
//
//  Update model, threading and handle discipline are unchanged from v2: CPU
//  shadow + dirty pages, deltas only, generational slots reject stale handles,
//  scene-registry ownership, render-thread only.
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>  // Sprite2DInstanceData / Sprite2DHandle / quantizePriority / status
#include <lux/engine/render/resources/memory/PagedGpuStream.hpp>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/transfer/TransferScheduler.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

namespace lux::render
{
    class SceneDescriptorArena;

    /// Instance kinds sharing the canvas key space. Values are FROZEN into keys —
    /// append only. (Tile / Particle arrive with their slices.)
    enum class ECanvas2DKind : std::uint8_t
    {
        Sprite     = 0,
        PixelField = 1,   ///< F2-09: R16_UNORM id mirror + palette lookup
        Tile       = 2,   ///< A2-02: R16_UNORM tile-index map + tileset atlas sample
        COUNT
    };

    inline constexpr std::uint32_t kCanvas2DSlotBits = 28;
    inline constexpr std::uint32_t kCanvas2DSlotMask = (1u << kCanvas2DSlotBits) - 1u;

    /// One contiguous same-kind segment of the global ascending-key order.
    /// `first` = offset within the kind's ORDER buffer (== firstInstance).
    /// `group` = the offscreen group this run belongs to (A2-04): the group-g
    /// pass kernel draws only its own runs; group 0 = the direct SceneColor
    /// pass. Runs are emitted group-major (all of group 0, then 1, …), each
    /// group internally in ascending key order.
    struct Canvas2DRun
    {
        std::uint8_t  kind;
        std::uint8_t  group;
        std::uint32_t first;
        std::uint32_t count;
    };

    class Canvas2DInstanceArena
    {
        struct Slot
        {
            std::uint32_t gen{0};
            std::uint32_t key_hi{0};
            std::uint8_t  group{0};   ///< A2-04 offscreen group (sprite kind only today)
            bool          alive{false};
            bool          visible{false};
        };

        /// Cold-path seam the shared machinery drives per kind (rebuild is
        /// key-change-frame only; transfers touch only dirty stores).
        class IKindStore
        {
        public:
            virtual ~IKindStore() = default;
            virtual void collectLiveKeys(std::vector<std::uint64_t>& out,
                                         std::uint64_t kind_field,
                                         std::uint8_t group) const = 0;
            virtual void resetOrderCursor() = 0;
            /// Write @p slot at the kind's next order position; returns that position.
            [[nodiscard]] virtual std::uint32_t appendOrdered(std::uint32_t slot) = 0;
            virtual void submitStreams(TransferScheduler& scheduler) = 0;
        };

        /// The generic per-kind store: slots + record stream + order subsequence +
        /// descriptor set. Typed mutators are exposed through the arena's per-kind
        /// façade (handlers stay wire-typed); everything order-related goes through
        /// the shared IKindStore seam.
        template <class Record>
        class KindStore final : public IKindStore
        {
        public:
            void init(DeviceContext* dev, std::uint32_t cap, std::uint32_t max_cap)
            {
                device_ctx_   = dev;
                max_capacity_ = std::min(std::max(max_cap, 1u), kCanvas2DSlotMask + 1u);
                capacity_     = std::min(std::max(cap, 1u), max_capacity_);
                records_.init(dev, capacity_);
                order_.init(dev, capacity_);
                slots_.resize(capacity_);
            }

            void shutdown()
            {
                records_.shutdown();
                order_.shutdown();
            }

            void setDeferredQueue(DeferredDestroyQueue* q) noexcept
            {
                records_.setDeferredQueue(q);
                order_.setDeferredQueue(q);
            }

            // ── typed op entry points ────────────────────────────────────────
            [[nodiscard]] ECanvas2DCreateStatus add(const Record& r, std::uint32_t key_hi,
                                                    bool visible, std::uint8_t group,
                                                    std::uint32_t& out_slot, std::uint32_t& out_gen)
            {
                std::uint32_t slot;
                if (!free_.empty()) { slot = free_.back(); free_.pop_back(); }
                else
                {
                    if (!ensureCapacity(slot_count_ + 1))
                        return ECanvas2DCreateStatus::CapacityExhausted;
                    slot = slot_count_++;
                }
                Slot& s   = slots_[slot];
                s.alive   = true;
                s.visible = visible;
                s.key_hi  = key_hi;
                s.group   = group;
                records_.at(slot) = r;
                records_.markDirty(slot);
                out_slot = slot;
                out_gen  = s.gen;
                return ECanvas2DCreateStatus::Ok;
            }

            /// Returns true when the removal affected the draw order (was visible).
            bool remove(std::uint32_t slot, std::uint32_t gen)
            {
                Slot* s = resolve(slot, gen);
                if (!s) return false;
                s->alive = false;
                ++s->gen;                    // every outstanding handle is now stale
                free_.push_back(slot);
                return s->visible;
            }

            [[nodiscard]] Record* resolveRecord(std::uint32_t slot, std::uint32_t gen)
            {
                return resolve(slot, gen) ? &records_.at(slot) : nullptr;
            }
            void markRecordDirty(std::uint32_t slot) { records_.markDirty(slot); }

            /// Returns true when the key actually changed (order rebuild needed).
            bool writeKey(std::uint32_t slot, std::uint32_t gen,
                          std::uint32_t key_hi, bool visible, std::uint8_t group)
            {
                Slot* s = resolve(slot, gen);
                if (!s || (s->key_hi == key_hi && s->visible == visible && s->group == group))
                    return false;
                s->key_hi  = key_hi;
                s->visible = visible;
                s->group   = group;
                return true;
            }

            // ── shared-machinery seam ────────────────────────────────────────
            void collectLiveKeys(std::vector<std::uint64_t>& out,
                                 std::uint64_t kind_field,
                                 std::uint8_t group) const override
            {
                for (std::uint32_t i = 0; i < slot_count_; ++i)
                    if (slots_[i].alive && slots_[i].visible && slots_[i].group == group)
                        out.push_back((static_cast<std::uint64_t>(slots_[i].key_hi) << 32)
                                      | kind_field | i);
            }
            void resetOrderCursor() override { order_cursor_ = 0; }
            [[nodiscard]] std::uint32_t appendOrdered(std::uint32_t slot) override
            {
                const std::uint32_t pos = order_cursor_++;
                order_.at(pos) = slot;
                order_.markDirty(pos);
                return pos;
            }

            void submitStreams(TransferScheduler& scheduler) override
            {
                const bool full = full_reupload_;
                if (slot_count_ == 0 ||
                    (!full && !records_.hasDirtyPages() && !order_.hasDirtyPages()))
                    return;

                rec_chunks_.clear();
                ord_chunks_.clear();
                const VkDeviceSize rec_bytes =
                    (full || records_.hasDirtyPages())
                        ? records_.collectUploadChunks(slot_count_, full, rec_chunks_)
                        : 0u;
                const VkDeviceSize ord_bytes =
                    ((full || order_.hasDirtyPages()) && order_cursor_ > 0)
                        ? order_.collectUploadChunks(order_cursor_, full, ord_chunks_)
                        : 0u;
                const VkDeviceSize total = rec_bytes + ord_bytes;
                if (total == 0u) { full_reupload_ = false; return; }

                auto stg = scheduler.allocateStaging(total);
                if (!stg) return;   // pages stay dirty → retried next tick

                auto*        dst    = static_cast<std::uint8_t*>(stg.mapped);
                VkDeviceSize offset = 0;
                const auto emit = [&](VkBuffer gpu, const auto& chunks)
                {
                    for (const auto& c : chunks)
                    {
                        std::memcpy(dst + offset, c.src, static_cast<std::size_t>(c.size));
                        scheduler.submitBufferCopy({
                            .src        = stg.buffer,
                            .src_offset = stg.srcOffset + offset,
                            .dst        = gpu,
                            .dst_offset = c.dst_offset,
                            .size       = c.size,
                            .domain     = EBufferDomain::Storage_VS,
                        });
                        offset += c.size;
                    }
                };
                emit(records_.buffer(), rec_chunks_);
                emit(order_.buffer(), ord_chunks_);
                records_.clearDirtyState();
                order_.clearDirtyState();
                full_reupload_ = false;
            }

            // ── descriptor set (binding 0 = records, binding 1 = order) ────────
            void createSet(DescriptorService* svc, DescriptorLayoutId layout,
                           SceneDescriptorArena* arena)
            {
                svc_          = svc;
                ds_layout_id_ = layout;
                ds_           = Canvas2DInstanceArena::allocateSet(arena, svc->layout(layout));
                refreshSet();
            }
            [[nodiscard]] VkDescriptorSet descriptorSet() const noexcept { return ds_; }
            [[nodiscard]] std::uint32_t   liveCount() const noexcept
            {
                return slot_count_ - static_cast<std::uint32_t>(free_.size());
            }

        private:
            [[nodiscard]] Slot* resolve(std::uint32_t slot, std::uint32_t gen) noexcept
            {
                if (slot >= slot_count_) return nullptr;
                Slot& s = slots_[slot];
                return (s.alive && s.gen == gen) ? &s : nullptr;
            }

            [[nodiscard]] bool ensureCapacity(std::uint32_t required)
            {
                if (required <= capacity_) return true;
                if (capacity_ >= max_capacity_) return false;
                std::uint32_t nc = std::max(capacity_ * 2u, 256u);
                while (nc < required && nc < max_capacity_) nc *= 2u;
                nc = std::min(nc, max_capacity_);
                if (nc < required) return false;
                if (!records_.reserve(nc) || !order_.reserve(nc))
                    return false;
                slots_.resize(nc);
                capacity_      = nc;
                full_reupload_ = true;   // reserve() discards GPU contents
                refreshSet();            // buffers changed under the set
                return true;
            }

            void refreshSet()
            {
                if (ds_ == VK_NULL_HANDLE) return;
                std::array<VkDescriptorBufferInfo, 2> infos{};
                infos[0] = {records_.buffer(), 0, VK_WHOLE_SIZE};
                infos[1] = {order_.buffer(), 0, VK_WHOLE_SIZE};
                std::array<VkWriteDescriptorSet, 2> writes{};
                for (std::uint32_t i = 0; i < 2; ++i)
                {
                    writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[i].dstSet          = ds_;
                    writes[i].dstBinding      = i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[i].pBufferInfo     = &infos[i];
                }
                vkUpdateDescriptorSets(device_ctx_->logicalDevice(), 2, writes.data(), 0, nullptr);
            }

            PagedGpuStream<Record>        records_;
            PagedGpuStream<std::uint32_t> order_;
            std::vector<Slot>             slots_;
            std::vector<std::uint32_t>    free_;
            std::vector<typename PagedGpuStream<Record>::UploadChunk>        rec_chunks_;
            std::vector<typename PagedGpuStream<std::uint32_t>::UploadChunk> ord_chunks_;

            std::uint32_t slot_count_{0};
            std::uint32_t capacity_{0};
            std::uint32_t max_capacity_{0};
            std::uint32_t order_cursor_{0};
            bool          full_reupload_{false};

            DeviceContext*     device_ctx_{nullptr};
            DescriptorService* svc_{nullptr};
            DescriptorLayoutId ds_layout_id_{kInvalidDescriptorLayoutId};
            VkDescriptorSet    ds_{VK_NULL_HANDLE};

            friend class Canvas2DInstanceArena;
        };

    public:
        struct InitInfo
        {
            DeviceContext*        device_context{nullptr};
            DescriptorService*    descriptor_svc{nullptr};
            SceneDescriptorArena* arena{nullptr};
            std::uint32_t         initial_capacity{4096};
            std::uint32_t         max_capacity{65536};
            std::uint32_t         offscreen_groups{0};   ///< A2-04 (clamped to kMaxCanvas2DGroups)
        };

        Canvas2DInstanceArena() = default;
        Canvas2DInstanceArena(const Canvas2DInstanceArena&) = delete;
        Canvas2DInstanceArena& operator=(const Canvas2DInstanceArena&) = delete;
        ~Canvas2DInstanceArena() { sprites_.shutdown(); }

        /// Idempotent (the feature calls it at every attach; the first one builds).
        void init(const InitInfo& info)
        {
            if (initialized_) return;
            device_ctx_ = info.device_context;
            svc_        = info.descriptor_svc;
            group_count_ = 1u + std::min(info.offscreen_groups, kMaxCanvas2DGroups);

            // ONE layout for every kind (2× storage, VS|FS, UPDATE_AFTER_BIND —
            // the engine set-1 shape), registered once; one set per kind store.
            {
                std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
                bindings[0].binding         = 0;
                bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[0].descriptorCount = 1;
                bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
                bindings[1] = bindings[0];
                bindings[1].binding         = 1;
                std::array<VkDescriptorBindingFlags, 2> bind_flags{
                    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};
                ds_layout_id_ = svc_->registerLayout({.bindings      = bindings,
                                                      .binding_flags = bind_flags,
                                                      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                                      .debug_name = "Canvas2DInstanceArena"});
            }

            sprites_.init(device_ctx_, info.initial_capacity, info.max_capacity);
            sprites_.createSet(svc_, ds_layout_id_, info.arena);
            // Fields are FEW per scene (a handful of chunk quads) — a small store.
            fields_.init(device_ctx_, 64, 4096);
            fields_.createSet(svc_, ds_layout_id_, info.arena);
            // Tilemaps too: one instance per whole map (A2-02).
            tiles_.init(device_ctx_, 64, 4096);
            tiles_.createSet(svc_, ds_layout_id_, info.arena);
            initialized_ = (sprites_.descriptorSet() != VK_NULL_HANDLE &&
                            fields_.descriptorSet() != VK_NULL_HANDLE &&
                            tiles_.descriptorSet() != VK_NULL_HANDLE);
        }

        void setDeferredQueue(DeferredDestroyQueue* q) noexcept
        {
            sprites_.setDeferredQueue(q);
            fields_.setDeferredQueue(q);
            tiles_.setDeferredQueue(q);
        }

        // ── sprite-kind op entry points (wire-typed; handlers stay unchanged) ──

        [[nodiscard]] ECanvas2DCreateStatus add(const Sprite2DInstanceData& data,
                                                float priority, bool visible,
                                                Sprite2DHandle& out,
                                                std::uint32_t group = 0)
        {
            out = {};
            if (!initialized_) return ECanvas2DCreateStatus::InvalidConfiguration;
            std::uint32_t slot, gen;
            const auto st = sprites_.add(data, quantizePriority(priority), visible,
                                         clampGroup(group), slot, gen);
            if (st != ECanvas2DCreateStatus::Ok) return st;
            if (visible) order_dirty_ = true;
            out = Sprite2DHandle{slot, gen};
            return st;
        }

        void remove(Sprite2DHandle h)
        {
            if (!initialized_ || h.is_null()) return;
            if (sprites_.remove(h.index, h.gen))
                order_dirty_ = true;
        }

        void writeTransform(Sprite2DHandle h, const float m[6])
        {
            if (!initialized_ || h.is_null()) return;
            if (auto* r = sprites_.resolveRecord(h.index, h.gen))
            {
                std::memcpy(r->m, m, sizeof(float) * 6);
                sprites_.markRecordDirty(h.index);   // order untouched — transforms never re-sort
            }
        }

        void writeVisual(Sprite2DHandle h, const float uv[4],
                         std::uint32_t tint, std::uint32_t texture_bindless)
        {
            if (!initialized_ || h.is_null()) return;
            if (auto* r = sprites_.resolveRecord(h.index, h.gen))
            {
                std::memcpy(r->uv, uv, sizeof(float) * 4);
                r->tint             = tint;
                r->texture_bindless = texture_bindless;
                sprites_.markRecordDirty(h.index);
            }
        }

        void writeKey(Sprite2DHandle h, float priority, bool visible, std::uint32_t group = 0)
        {
            if (!initialized_ || h.is_null()) return;
            if (sprites_.writeKey(h.index, h.gen, quantizePriority(priority), visible,
                                  clampGroup(group)))
                order_dirty_ = true;
        }

        void setEnabled(bool e) noexcept { enabled_ = e; }

        // ── PixelField-kind op entry points (F2-09; same grammar as sprites) ────

        [[nodiscard]] ECanvas2DCreateStatus addField(const PixelField2DInstanceData& data,
                                                     float priority, bool visible,
                                                     PixelFieldInstanceHandle& out)
        {
            out = {};
            if (!initialized_) return ECanvas2DCreateStatus::InvalidConfiguration;
            std::uint32_t slot, gen;
            const auto st = fields_.add(data, quantizePriority(priority), visible, 0, slot, gen);
            if (st != ECanvas2DCreateStatus::Ok) return st;
            if (visible) order_dirty_ = true;
            out = PixelFieldInstanceHandle{slot, gen};
            return st;
        }

        void removeField(PixelFieldInstanceHandle h)
        {
            if (!initialized_ || h.is_null()) return;
            if (fields_.remove(h.index, h.gen))
                order_dirty_ = true;
        }

        void writeFieldTransform(PixelFieldInstanceHandle h, const float m[6])
        {
            if (!initialized_ || h.is_null()) return;
            if (auto* r = fields_.resolveRecord(h.index, h.gen))
            {
                std::memcpy(r->m, m, sizeof(float) * 6);
                fields_.markRecordDirty(h.index);
            }
        }

        void writeFieldKey(PixelFieldInstanceHandle h, float priority, bool visible)
        {
            if (!initialized_ || h.is_null()) return;
            if (fields_.writeKey(h.index, h.gen, quantizePriority(priority), visible, 0))
                order_dirty_ = true;
        }

        // ── Tile-kind op entry points (A2-02; same grammar as sprites/fields) ───

        [[nodiscard]] ECanvas2DCreateStatus addTile(const Tile2DInstanceData& data,
                                                    float priority, bool visible,
                                                    Tile2DInstanceHandle& out)
        {
            out = {};
            if (!initialized_) return ECanvas2DCreateStatus::InvalidConfiguration;
            std::uint32_t slot, gen;
            const auto st = tiles_.add(data, quantizePriority(priority), visible, 0, slot, gen);
            if (st != ECanvas2DCreateStatus::Ok) return st;
            if (visible) order_dirty_ = true;
            out = Tile2DInstanceHandle{slot, gen};
            return st;
        }

        void removeTile(Tile2DInstanceHandle h)
        {
            if (!initialized_ || h.is_null()) return;
            if (tiles_.remove(h.index, h.gen))
                order_dirty_ = true;
        }

        void writeTileTransform(Tile2DInstanceHandle h, const float m[6])
        {
            if (!initialized_ || h.is_null()) return;
            if (auto* r = tiles_.resolveRecord(h.index, h.gen))
            {
                std::memcpy(r->m, m, sizeof(float) * 6);
                tiles_.markRecordDirty(h.index);
            }
        }

        void writeTileKey(Tile2DInstanceHandle h, float priority, bool visible)
        {
            if (!initialized_ || h.is_null()) return;
            if (tiles_.writeKey(h.index, h.gen, quantizePriority(priority), visible, 0))
                order_dirty_ = true;
        }

        // ── transfer phase (scene transfer contributor) ─────────────────────────

        void submitTransfers(TransferScheduler& scheduler)
        {
            if (!initialized_) return;
            if (order_dirty_)
            {
                rebuildOrder();
                order_dirty_ = false;
            }
            for (auto* s : stores())
                s->submitStreams(scheduler);
        }

        // ── draw side / observability ───────────────────────────────────────────

        [[nodiscard]] bool initialized() const noexcept { return initialized_; }
        /// The run table of the current order (empty ⇒ nothing to draw). The kernel
        /// walks it; a single-kind scene is exactly ONE run.
        [[nodiscard]] std::span<const Canvas2DRun> runs() const noexcept
        {
            return enabled_ ? std::span<const Canvas2DRun>{runs_} : std::span<const Canvas2DRun>{};
        }
        [[nodiscard]] VkDescriptorSet descriptorSet(ECanvas2DKind kind) const noexcept
        {
            switch (kind)
            {
            case ECanvas2DKind::Sprite:     return sprites_.descriptorSet();
            case ECanvas2DKind::PixelField: return fields_.descriptorSet();
            case ECanvas2DKind::Tile:       return tiles_.descriptorSet();
            default:                        return VK_NULL_HANDLE;
            }
        }
        [[nodiscard]] std::uint32_t liveCount() const noexcept
        {
            return sprites_.liveCount() + fields_.liveCount() + tiles_.liveCount();
        }
        [[nodiscard]] std::uint64_t orderRebuilds() const noexcept { return order_rebuilds_; }

        /// Defined next to the feature (needs the complete SceneDescriptorArena type).
        static VkDescriptorSet allocateSet(SceneDescriptorArena* arena, VkDescriptorSetLayout layout);

    private:
        [[nodiscard]] std::array<IKindStore*, 3> stores() noexcept { return {&sprites_, &fields_, &tiles_}; }

        [[nodiscard]] std::uint8_t clampGroup(std::uint32_t group) const noexcept
        {
            return static_cast<std::uint8_t>(group < group_count_ ? group : 0u);
        }

        void rebuildOrder()
        {
            // Group-major: each group's keys sort independently; every kind's
            // order buffer is ONE sequence whose cursor runs across groups, so
            // firstInstance stays a plain offset. Group 0 first (the direct
            // pass), then each offscreen group.
            const auto sts = stores();
            for (auto* s : sts)
                s->resetOrderCursor();
            runs_.clear();
            for (std::uint8_t g = 0; g < group_count_; ++g)
            {
                key_scratch_.clear();
                for (std::size_t k = 0; k < sts.size(); ++k)
                    sts[k]->collectLiveKeys(key_scratch_,
                                            static_cast<std::uint64_t>(k) << kCanvas2DSlotBits, g);
                std::sort(key_scratch_.begin(), key_scratch_.end());
                for (const std::uint64_t key : key_scratch_)
                {
                    const auto kind = static_cast<std::uint8_t>((key >> kCanvas2DSlotBits) & 0xFu);
                    const auto slot = static_cast<std::uint32_t>(key & kCanvas2DSlotMask);
                    const std::uint32_t pos = sts[kind]->appendOrdered(slot);
                    if (runs_.empty() || runs_.back().kind != kind || runs_.back().group != g)
                        runs_.push_back(Canvas2DRun{kind, g, pos, 1});
                    else
                        ++runs_.back().count;
                }
            }
            ++order_rebuilds_;
        }

        KindStore<Sprite2DInstanceData>      sprites_;
        KindStore<PixelField2DInstanceData>  fields_;
        KindStore<Tile2DInstanceData>        tiles_;

        std::vector<std::uint64_t> key_scratch_;
        std::vector<Canvas2DRun>   runs_;
        std::uint32_t              group_count_{1};   ///< 1 + declared offscreen groups
        std::uint64_t              order_rebuilds_{0};
        bool                       order_dirty_{false};
        bool                       enabled_{true};
        bool                       initialized_{false};

        DeviceContext*     device_ctx_{nullptr};
        DescriptorService* svc_{nullptr};
        DescriptorLayoutId ds_layout_id_{kInvalidDescriptorLayoutId};
    };

} // namespace lux::render
