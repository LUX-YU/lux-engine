#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/resources/mesh/InstanceSlotRegistry.hpp>
#include <lux/engine/render/resources/lifecycle/VRAMBudgetGuard.hpp>
#include <lux/engine/render/transfer/TransferScheduler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace lux::render
{
    // =========================================================================
    //  Lifecycle
    // =========================================================================
    InstanceResources::~InstanceResources()
    {
        if (initialized_)
            shutdown();
    }

    void InstanceResources::init(const InitInfo &info)
    {
        if (initialized_)
            return; // idempotent: safe to call from multiple Features
        initialized_ = true;

        device_ctx_ = info.device_context;
        descriptor_svc_ = info.descriptor_svc;
        arena_ = info.arena;
        max_capacity_ = info.max_capacity;
        capacity_ = info.initial_capacity;
        slot_count_ = 0;
        full_rebuild_ = true;

        // ── CPU staging + GPU streams ──
        transform_stream_.init(device_ctx_, capacity_);
        prev_transform_stream_.init(device_ctx_, capacity_);
        property_stream_.init(device_ctx_, capacity_);
        cull_meta_stream_.init(device_ctx_, capacity_);
        mesh_section_table_.init(device_ctx_, capacity_);
        local_bsphere_.resize(capacity_, {0.f, 0.f, 0.f, 0.f});
        registry_ = std::make_unique<InstanceSlotRegistry>();
        registry_->init(capacity_);

        // ── Descriptor set layout (binding 0 = Transform, binding 1 = Property) ──
        // Layout must be identical to GeneralDescriptorSetLayout set 1 so that
        // other features (Skybox, DepthPrepass, Particle) can bind the
        // descriptor set at set 1 with pipeline layouts built from the
        // General layout.
        {
            std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

            std::array<VkDescriptorBindingFlags, 2> bind_flags{
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};

            ds_layout_id_ = descriptor_svc_->registerLayout({.bindings = bindings,
                                                             .binding_flags = bind_flags,
                                                             .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                                             .debug_name = "InstanceResources"});
        }

        // ── Allocate (from the per-scene arena) + write initial descriptor set ──
        ds_ = arena_->allocate(descriptor_svc_->layout(ds_layout_id_));
        refreshDescriptorSet();
    }

    void InstanceResources::shutdown()
    {
        if (!initialized_)
            return;

        transform_stream_.shutdown();
        prev_transform_stream_.shutdown();
        property_stream_.shutdown();
        cull_meta_stream_.shutdown();
        mesh_section_table_.shutdown();

        for (uint32_t i = 0; i < kMdcInfoRingSize; ++i)
        {
            if (mdc_info_buffers_[i] != VK_NULL_HANDLE)
                vmaDestroyBuffer(device_ctx_->vmaAllocator(), mdc_info_buffers_[i], mdc_info_allocs_[i]);
            mdc_info_buffers_[i] = VK_NULL_HANDLE;
            mdc_info_allocs_[i]  = nullptr;
            mdc_info_mapped_[i]  = nullptr;
            mdc_info_sizes_[i]   = 0;
        }
        mdc_info_ring_cursor_        = 0;
        mdc_info_current_slot_       = 0;
        mdc_info_last_upload_serial_ = ~0ull;
        mdc_table_.clear();

        // DS and layout are pool-managed by DescriptorService.
        local_bsphere_.clear();
        if (registry_)
            registry_->shutdown();
        registry_.reset();

        initialized_ = false;
    }

    // =========================================================================
    //  MDC info upload
    // =========================================================================

    void InstanceResources::uploadMdcInfo()
    {
        mdc_table_.buildOffsets();
        const auto &gpu_data = mdc_table_.gpuData();
        assert(!gpu_data.empty() && "buildOffsets must always produce at least a safety sentinel");

        const VkDeviceSize required = static_cast<VkDeviceSize>(gpu_data.size()) * sizeof(uint32_t);

        // Advance the ring once per graph compile. Multiple cull passes in the
        // SAME compile (Forward + Deferred, multiple views) call uploadMdcInfo
        // with the same frame serial and must share one slot; the next compile
        // (new serial) takes a fresh slot so in-flight frames keep reading the
        // slot they were recorded with.
        if (mdc_info_current_serial_ != mdc_info_last_upload_serial_)
        {
            mdc_info_ring_cursor_       = (mdc_info_ring_cursor_ + 1u) % kMdcInfoRingSize;
            mdc_info_last_upload_serial_ = mdc_info_current_serial_;
        }
        const uint32_t slot = mdc_info_ring_cursor_;
        mdc_info_current_slot_ = slot;

        // Grow this slot's buffer if the new offsets no longer fit. The slot was
        // last written kMdcInfoRingSize (>= FIF+1) compiles ago, so its prior
        // allocation is GPU-idle; still route the free through the deferred queue
        // for safety.
        if (required > mdc_info_sizes_[slot])
        {
            if (mdc_info_buffers_[slot] != VK_NULL_HANDLE)
            {
                if (deferred_queue_)
                    deferred_queue_->retireBuffer(mdc_info_buffers_[slot], mdc_info_allocs_[slot]);
                else
                    vmaDestroyBuffer(device_ctx_->vmaAllocator(), mdc_info_buffers_[slot], mdc_info_allocs_[slot]);
            }

            mdc_info_buffers_[slot] = VK_NULL_HANDLE;
            mdc_info_allocs_[slot]  = nullptr;
            mdc_info_mapped_[slot]  = nullptr;
            mdc_info_sizes_[slot]   = 0;

            VkBuffer      buf{VK_NULL_HANDLE};
            VmaAllocation alloc{nullptr};
            void*         mapped{nullptr};
            createGpuBufferVmaBuffer(
                device_ctx_->vmaAllocator(),
                required,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                /*cpu_writable=*/true,
                &buf,
                &alloc,
                &mapped);

            // On allocation failure leave the slot null instead of publishing a
            // half-built (already-retired) handle to the render graph.
            if (buf == VK_NULL_HANDLE || mapped == nullptr)
            {
                if (buf != VK_NULL_HANDLE)
                    vmaDestroyBuffer(device_ctx_->vmaAllocator(), buf, alloc);
                return;
            }

            mdc_info_buffers_[slot] = buf;
            mdc_info_allocs_[slot]  = alloc;
            mdc_info_mapped_[slot]  = mapped;
            mdc_info_sizes_[slot]   = required;
        }

        // Write via persistent mapping. Host-coherent + the implicit host-write
        // barrier at queue submit make this visible to this frame's cull dispatch.
        if (mdc_info_mapped_[slot])
            std::memcpy(mdc_info_mapped_[slot], gpu_data.data(), required);
    }

    // =========================================================================
    //  Slot management
    // =========================================================================

    InstanceSlot InstanceResources::allocate()
    {
        if (!registry_)
            return InstanceSlot::invalid();

        if (registry_->needsGrowForAllocate())
        {
            const uint32_t required = registry_->slotCount() + 1u;
            if (!ensureCapacity(required))
                return InstanceSlot::invalid();
        }

        const InstanceSlot slot = registry_->allocate();
        if (!slot)
            return InstanceSlot::invalid();

        slot_count_ = registry_->slotCount();

        auto &prop = property_stream_.at(slot.index);
        auto &cull = cull_meta_stream_.at(slot.index);
        cull.bsphere[0] = 0.0f;
        cull.bsphere[1] = 0.0f;
        cull.bsphere[2] = 0.0f;
        cull.bsphere[3] = 0.0f;
        cull.lod_count = 0u;
        prop.transform_index = slot.index;
        prop.object_id = kInvalidObjectId;
        prop.pass_and_geometry =
            static_cast<uint32_t>(kPassMaskOpaqueDefault) | (static_cast<uint32_t>(EGeometryKind::StaticMesh) << 16u);
        local_bsphere_[slot.index] = {0.f, 0.f, 0.f, 0.f};

        // New slot needs all streams uploaded.
        markTransformDirty(slot);
        markPrevTransformDirty(slot);
        markPropertyDirty(slot);
        markCullDirty(slot);

        return slot;
    }

    void InstanceResources::unregisterInstanceLods(InstanceCullMeta& cull)
    {
        for (uint32_t i = 0; i < cull.lod_count && i < 4u; ++i)
        {
            const uint32_t mdc = cull.lod_mdc[i];
            // The LOD's mesh-section id lives on its MDC entry — read it back BEFORE
            // unregistering the MDC (which may recycle the entry).
            if (mdc < mdc_table_.count())
                mesh_section_table_.unregisterSection(mdc_table_.entries()[mdc].section_id);
            mdc_table_.unregisterInstance(mdc);
        }
        cull.lod_count = 0u;
    }

    void InstanceResources::free(InstanceSlot slot)
    {
        if (!registry_ || !registry_->isAlive(slot))
            return;

        auto &cull = cull_meta_stream_.at(slot.index);
        unregisterInstanceLods(cull);

        if (!registry_->free(slot))
            return;

        slot_count_ = registry_->slotCount();

        // Tombstone convention: bsphere.w < 0.
        cull.bsphere[3] = -1.0f;
        markCullDirty(slot);
    }

    bool InstanceResources::isAlive(InstanceSlot slot) const noexcept
    {
        return registry_ && registry_->isAlive(slot);
    }

    RenderObjectHandle InstanceResources::allocateObject()
    {
        if (!registry_)
            return RenderObjectHandle::invalid();

        if (registry_->needsGrowForAllocate())
        {
            const uint32_t required = registry_->slotCount() + 1u;
            if (!ensureCapacity(required))
                return RenderObjectHandle::invalid();
        }

        const RenderObjectHandle handle = registry_->allocateObject();
        if (!handle)
            return RenderObjectHandle::invalid();

        const InstanceSlot slot = registry_->resolveSlot(handle);
        if (!slot)
            return RenderObjectHandle::invalid();

        slot_count_ = registry_->slotCount();

        auto &prop = property_stream_.at(slot.index);
        auto &cull = cull_meta_stream_.at(slot.index);
        cull.bsphere[0] = 0.0f;
        cull.bsphere[1] = 0.0f;
        cull.bsphere[2] = 0.0f;
        cull.bsphere[3] = 0.0f;
        cull.lod_count = 0u;
        prop.object_id = handle.index;
        prop.transform_index = slot.index;
        prop.pass_and_geometry =
            static_cast<uint32_t>(kPassMaskOpaqueDefault) | (static_cast<uint32_t>(EGeometryKind::StaticMesh) << 16u);
        local_bsphere_[slot.index] = {0.f, 0.f, 0.f, 0.f};

        markTransformDirty(slot);
        markPrevTransformDirty(slot);
        markPropertyDirty(slot);
        markCullDirty(slot);

        return handle;
    }

    void InstanceResources::freeObject(RenderObjectHandle handle)
    {
        free(resolveSlot(handle));
    }

    bool InstanceResources::isAlive(RenderObjectHandle handle) const noexcept
    {
        return registry_ && registry_->isAlive(handle);
    }

    uint32_t InstanceResources::generation(InstanceSlot slot) const noexcept
    {
        return registry_ ? registry_->generation(slot) : 0u;
    }

    InstanceSlot InstanceResources::resolveSlot(RenderObjectHandle handle) const noexcept
    {
        return registry_ ? registry_->resolveSlot(handle) : InstanceSlot::invalid();
    }

    RenderObjectHandle InstanceResources::handleForSlot(InstanceSlot slot) const noexcept
    {
        return registry_ ? registry_->handleForSlot(slot) : RenderObjectHandle::invalid();
    }

    InstanceSlot InstanceResources::reserveSlot(uint32_t index)
    {
        if (index >= capacity_)
        {
            if (!ensureCapacity(index + 1))
                return InstanceSlot::invalid();
        }

        if (!registry_)
            return InstanceSlot::invalid();

        const InstanceSlot requested{index};
        const bool was_alive = registry_->isAlive(requested);
        const InstanceSlot slot = registry_->reserveSlot(index);
        if (!slot)
            return InstanceSlot::invalid();

        slot_count_ = registry_->slotCount();

        // Reserve-path callers may claim previously unused slots directly.
        // Initialize canonical defaults so later free()/writeCullMeta() cannot
        // accidentally touch stale mesh-section references.
        if (!was_alive)
        {
            auto &prop = property_stream_.at(slot.index);
            auto &cull = cull_meta_stream_.at(slot.index);

            cull.bsphere[0] = 0.0f;
            cull.bsphere[1] = 0.0f;
            cull.bsphere[2] = 0.0f;
            cull.bsphere[3] = 0.0f;
            cull.lod_count = 0u;

            prop.transform_index = slot.index;
            prop.object_id = kInvalidObjectId;
            prop.pass_and_geometry =
                static_cast<uint32_t>(kPassMaskOpaqueDefault) | (static_cast<uint32_t>(EGeometryKind::StaticMesh) << 16u);

            local_bsphere_[slot.index] = {0.f, 0.f, 0.f, 0.f};

            markTransformDirty(slot);
            markPrevTransformDirty(slot);
            markPropertyDirty(slot);
            markCullDirty(slot);
        }

        return slot;
    }

    bool InstanceResources::shouldCompact() const noexcept
    {
        if (!registry_)
            return false;
        if (compaction_requested_)
            return true;
        if (capacity_ < 8192u || registry_->slotCount() < 8192u)
            return false;
        return (registry_->freeCount() * 100u) >= (capacity_ * 30u);
    }

    void InstanceResources::compactSlots()
    {
        if (!registry_)
            return;

        auto compact_result = registry_->compact();
        const uint32_t live_count = compact_result.live_count;

        transform_stream_.compact(compact_result.old_to_new, live_count);
        prev_transform_stream_.compact(compact_result.old_to_new, live_count);
        property_stream_.compact(compact_result.old_to_new, live_count);
        cull_meta_stream_.compact(compact_result.old_to_new, live_count);

        if (!compact_result.old_to_new.empty())
        {
            std::vector<std::array<float, 4>> compact_local_bsphere(live_count, {0.f, 0.f, 0.f, 0.f});
            const uint32_t old_count =
                static_cast<uint32_t>(std::min(compact_result.old_to_new.size(), local_bsphere_.size()));
            for (uint32_t old_slot = 0; old_slot < old_count; ++old_slot)
            {
                const uint32_t new_slot = compact_result.old_to_new[old_slot];
                if (new_slot == InstanceSlotRegistry::kInvalidDensePos || new_slot >= live_count)
                    continue;
                compact_local_bsphere[new_slot] = local_bsphere_[old_slot];
            }

            for (uint32_t slot = 0; slot < live_count; ++slot)
                local_bsphere_[slot] = compact_local_bsphere[slot];

            const uint32_t old_slot_count = static_cast<uint32_t>(compact_result.old_to_new.size());
            for (uint32_t slot = live_count; slot < old_slot_count && slot < cull_meta_stream_.cpuData().size(); ++slot)
            {
                auto &cull = cull_meta_stream_.at(slot);
                cull.bsphere[3] = -1.0f;
                cull.lod_count = 0u;
            }
        }

        for (uint32_t slot = 0; slot < live_count; ++slot)
            property_stream_.at(slot).transform_index = slot;

        slot_count_ = registry_->slotCount();
        compaction_requested_ = false;
        full_rebuild_ = true;
    }

    bool InstanceResources::ensureCapacity(uint32_t required)
    {
        if (required <= capacity_)
            return true;

        uint32_t new_cap = capacity_;
        while (new_cap < required)
            new_cap = new_cap + new_cap / 2; // 1.5× growth

        // Auto-lift max_capacity — VRAM budget check below is the real limiter.
        if (new_cap > max_capacity_)
            max_capacity_ = new_cap;

        // Pre-flight VRAM budget check: estimate bytes for 3 GPU_ONLY buffers.
        {
            const VkDeviceSize bytes_needed =
                VkDeviceSize(new_cap) * (sizeof(InstanceTransform) + sizeof(InstanceTransformPrev) + sizeof(InstanceProperty) + sizeof(InstanceCullMeta));
            VRAMBudgetGuard budget(device_ctx_->vmaAllocator());
            if (!budget.canAllocate(bytes_needed))
                return false;
        }

        // Reserve each GPU stream in sequence.
        for (uint32_t i = 0; i < 4u; ++i)
        {
            bool ok = false;
            switch (i)
            {
            case 0:
                ok = transform_stream_.reserve(new_cap);
                break;
            case 1:
                ok = prev_transform_stream_.reserve(new_cap);
                break;
            case 2:
                ok = property_stream_.reserve(new_cap);
                break;
            case 3:
                ok = cull_meta_stream_.reserve(new_cap);
                break;
            default:
                break;
            }
            if (!ok)
            {
                if (i > 0)
                {
                    // Partial growth: streams 0..i-1 grew (PagedGpuStream::reserve
                    // discards their GPU contents) but stream i failed. Force a full
                    // rebuild so the next submitTransfers restores the grown streams
                    // from the intact CPU mirrors — otherwise they render garbage
                    // transforms (only later-dirtied pages would re-upload). (C-3)
                    full_rebuild_ = true;
                    refreshDescriptorSet();
                }
                return false;
            }
        }

        // All stream allocations succeeded — grow CPU-side auxiliary containers.
        local_bsphere_.resize(new_cap, {0.f, 0.f, 0.f, 0.f});
        if (registry_)
            registry_->resizeCapacity(new_cap);

        capacity_ = new_cap;
        slot_count_ = registry_ ? registry_->slotCount() : slot_count_;
        full_rebuild_ = true;
        refreshDescriptorSet();
        return true;
    }

    // =========================================================================
    //  Per-stream writes
    // =========================================================================

    void InstanceResources::writeTransform(InstanceSlot slot, const InstanceTransform &xform)
    {
        std::memcpy(
            prev_transform_stream_.at(slot.index).world_prev,
            transform_stream_.at(slot.index).world,
            sizeof(transform_stream_.at(slot.index).world));
        markPrevTransformDirty(slot);
        transform_stream_.at(slot.index) = xform;
        markTransformDirty(slot);
        recomputeWorldBsphere(slot);
    }

    void InstanceResources::writePrevTransform(InstanceSlot slot, const InstanceTransformPrev &xform_prev)
    {
        prev_transform_stream_.at(slot.index) = xform_prev;
        markPrevTransformDirty(slot);
    }

    void InstanceResources::setLocalBsphere(InstanceSlot slot, float cx, float cy, float cz, float r)
    {
        local_bsphere_[slot.index] = {cx, cy, cz, r};
        recomputeWorldBsphere(slot);
    }

    void InstanceResources::recomputeWorldBsphere(InstanceSlot slot)
    {
        const auto &lb = local_bsphere_[slot.index];
        const float *M = transform_stream_.at(slot.index).world; // column-major 4x4

        // Transform local center by world matrix: world_center = M * (cx, cy, cz, 1)
        float cx = lb[0], cy = lb[1], cz = lb[2], r = lb[3];
        float wx = M[0] * cx + M[4] * cy + M[8] * cz + M[12];
        float wy = M[1] * cx + M[5] * cy + M[9] * cz + M[13];
        float wz = M[2] * cx + M[6] * cy + M[10] * cz + M[14];

        // Conservative radius: scale by max column norm of upper-left 3x3.
        // Use squared norms + single sqrtf on the max to avoid 3 sqrt calls.
        float sq0 = M[0] * M[0] + M[1] * M[1] + M[2] * M[2];
        float sq1 = M[4] * M[4] + M[5] * M[5] + M[6] * M[6];
        float sq2 = M[8] * M[8] + M[9] * M[9] + M[10] * M[10];
        float max_scale = std::sqrt(std::max({sq0, sq1, sq2}));

        auto &meta = cull_meta_stream_.at(slot.index);
        meta.bsphere[0] = wx;
        meta.bsphere[1] = wy;
        meta.bsphere[2] = wz;
        meta.bsphere[3] = r * max_scale;
        markCullDirty(slot);
    }

    void InstanceResources::writeProperty(InstanceSlot slot, const InstanceProperty &prop)
    {
        property_stream_.at(slot.index) = prop;
        markPropertyDirty(slot);
    }

    void InstanceResources::writeCullMeta(InstanceSlot slot, const InstanceCullMeta &meta)
    {
        auto &current = cull_meta_stream_.at(slot.index);
        // Overwriting a live registration → release the previous LODs' MDCs +
        // sections first (the new ones were already registered by the caller).
        if (current.lod_count != 0u)
            unregisterInstanceLods(current);

        current = meta;
        // World-space bsphere is derived data from local_bsphere_ + transform.
        // Recompute here so caller write order cannot accidentally zero it out.
        recomputeWorldBsphere(slot);
    }

    InstanceTransform &InstanceResources::transformAt(InstanceSlot slot) noexcept
    {
        return transform_stream_.at(slot.index);
    }

    InstanceTransformPrev &InstanceResources::prevTransformAt(InstanceSlot slot) noexcept
    {
        return prev_transform_stream_.at(slot.index);
    }

    InstanceProperty &InstanceResources::propertyAt(InstanceSlot slot) noexcept
    {
        return property_stream_.at(slot.index);
    }

    const InstanceProperty &InstanceResources::propertyAt(InstanceSlot slot) const noexcept
    {
        return property_stream_.at(slot.index);
    }

    InstanceCullMeta &InstanceResources::cullMetaAt(InstanceSlot slot) noexcept
    {
        return cull_meta_stream_.at(slot.index);
    }

    const InstanceCullMeta &InstanceResources::cullMetaAt(InstanceSlot slot) const noexcept
    {
        return cull_meta_stream_.at(slot.index);
    }

    void InstanceResources::markTransformDirty(InstanceSlot slot)
    {
        transform_stream_.markDirty(slot.index);
    }

    void InstanceResources::markPrevTransformDirty(InstanceSlot slot)
    {
        prev_transform_stream_.markDirty(slot.index);
    }

    void InstanceResources::markPropertyDirty(InstanceSlot slot)
    {
        property_stream_.markDirty(slot.index);
    }

    void InstanceResources::markCullDirty(InstanceSlot slot)
    {
        cull_meta_stream_.markDirty(slot.index);
    }

    void InstanceResources::setRenderState(InstanceSlot slot, EGeometryKind geometry_kind, PassMask pass_mask)
    {
        auto &prop = property_stream_.at(slot.index);
        prop.pass_and_geometry =
            static_cast<uint32_t>(pass_mask) | (static_cast<uint32_t>(geometry_kind) << 16u);
        markPropertyDirty(slot);
    }

    // =========================================================================
    //  Frame lifecycle
    // =========================================================================

    void InstanceResources::beginFrame()
    {
        // Dirty lists are NOT cleared here — sync handlers may dirty slots
        // between beginFrame() and the actual upload.
    }

    // =========================================================================\n//  Transfer Scheduler path\n// =========================================================================

    void InstanceResources::submitTransfers(TransferScheduler &scheduler)
    {
        if (shouldCompact())
            compactSlots();

        if (slot_count_ == 0)
            return;

        const bool has_work =
            full_rebuild_ || transform_stream_.hasDirtyPages() || prev_transform_stream_.hasDirtyPages() || property_stream_.hasDirtyPages() || cull_meta_stream_.hasDirtyPages() || mesh_section_table_.hasWork();
        if (!has_work)
            return;

        const bool full_upload = full_rebuild_;
        const bool upload_transform = full_upload || transform_stream_.hasDirtyPages();
        const bool upload_prev_transform = full_upload || prev_transform_stream_.hasDirtyPages();
        const bool upload_property = full_upload || property_stream_.hasDirtyPages();
        const bool upload_cull = full_upload || cull_meta_stream_.hasDirtyPages();
        const bool upload_section = full_upload || mesh_section_table_.hasWork();

        // ── Collect chunks from all dirty instance streams ──────────────
        // Reuse the member scratch (cleared here) instead of fresh per-tick
        // vectors. collectUploadChunks only appends, so clear() before each. (P-5)
        xform_chunks_.clear();
        prev_xform_chunks_.clear();
        prop_chunks_.clear();
        cull_chunks_.clear();

        const VkDeviceSize xform_bytes =
            (upload_transform && transform_stream_.buffer() != VK_NULL_HANDLE)
                ? transform_stream_.collectUploadChunks(slot_count_, full_upload, xform_chunks_)
                : 0u;
        const VkDeviceSize prev_xform_bytes =
            (upload_prev_transform && prev_transform_stream_.buffer() != VK_NULL_HANDLE)
                ? prev_transform_stream_.collectUploadChunks(slot_count_, full_upload, prev_xform_chunks_)
                : 0u;
        const VkDeviceSize prop_bytes =
            (upload_property && property_stream_.buffer() != VK_NULL_HANDLE)
                ? property_stream_.collectUploadChunks(slot_count_, full_upload, prop_chunks_)
                : 0u;
        const VkDeviceSize cull_bytes =
            (upload_cull && cull_meta_stream_.buffer() != VK_NULL_HANDLE)
                ? cull_meta_stream_.collectUploadChunks(slot_count_, full_upload, cull_chunks_)
                : 0u;

        const VkDeviceSize total_instance_bytes =
            xform_bytes + prev_xform_bytes + prop_bytes + cull_bytes;

        // ── Allocate unified staging and submit copy requests ───────────
        // instance_uploaded gates the dirty/rebuild clear below: only a real
        // upload (or a no-op with zero bytes) clears state. A failed staging
        // allocation leaves pages dirty / full_rebuild_ set so the next frame
        // retries instead of silently shipping stale/zero GPU data.
        bool instance_uploaded = (total_instance_bytes == 0u);
        if (total_instance_bytes > 0u)
        {
            auto stg = scheduler.allocateStaging(total_instance_bytes);
            if (stg)
            {
                auto *dst = static_cast<uint8_t *>(stg.mapped);
                VkDeviceSize staging_offset = 0u;

                auto emitStreamCopies = [&](VkBuffer gpu_buf,
                                            const auto &chunks,
                                            EBufferDomain domain)
                {
                    if (gpu_buf == VK_NULL_HANDLE || chunks.empty())
                        return;
                    for (const auto &chunk : chunks)
                    {
                        std::memcpy(dst + staging_offset, chunk.src,
                                    static_cast<size_t>(chunk.size));
                        scheduler.submitBufferCopy({
                            .src = stg.buffer,
                            .src_offset = stg.srcOffset + staging_offset,
                            .dst = gpu_buf,
                            .dst_offset = chunk.dst_offset,
                            .size = chunk.size,
                            .domain = domain,
                        });
                        staging_offset += chunk.size;
                    }
                };

                emitStreamCopies(transform_stream_.buffer(), xform_chunks_, EBufferDomain::Storage_VS);
                emitStreamCopies(prev_transform_stream_.buffer(), prev_xform_chunks_, EBufferDomain::Storage_VS);
                emitStreamCopies(property_stream_.buffer(), prop_chunks_, EBufferDomain::Storage_All);
                emitStreamCopies(cull_meta_stream_.buffer(), cull_chunks_, EBufferDomain::Storage_CS);
                instance_uploaded = true;
            }
        }

        // Clear each uploaded stream's page-dirty state ONLY on success.
        // Without this, pages stay dirty forever and every stream re-uploads in
        // full every frame — silently defeating the incremental page-dirty
        // design (the single largest steady-state per-frame waste in the module).
        if (instance_uploaded)
        {
            // Mirror the collection conditions exactly: only clear a stream that
            // was actually collected (buffer present), so a not-yet-allocated
            // stream keeps its dirty pages for the next attempt.
            if (upload_transform && transform_stream_.buffer() != VK_NULL_HANDLE)
                transform_stream_.clearDirtyState();
            if (upload_prev_transform && prev_transform_stream_.buffer() != VK_NULL_HANDLE)
                prev_transform_stream_.clearDirtyState();
            if (upload_property && property_stream_.buffer() != VK_NULL_HANDLE)
                property_stream_.clearDirtyState();
            if (upload_cull && cull_meta_stream_.buffer() != VK_NULL_HANDLE)
                cull_meta_stream_.clearDirtyState();
        }

        // ── MeshSectionTable upload via scheduler ───────────────────────
        if (upload_section)
        {
            if (full_upload)
                mesh_section_table_.markFullRebuild();
            mesh_section_table_.submitTransfers(scheduler);
        }

        // Clear full_rebuild_ only when the instance streams actually uploaded;
        // a failed staging alloc must keep it set so the next frame re-pushes
        // everything (the reallocated GPU buffers hold no valid data yet).
        if (full_upload && instance_uploaded)
            full_rebuild_ = false;
    }

    // =========================================================================
    //  Descriptor management
    // =========================================================================

    void InstanceResources::refreshDescriptorSet()
    {
        if (ds_ == VK_NULL_HANDLE)
            return;

        VkDevice device = device_ctx_->logicalDevice();

        std::array<VkDescriptorBufferInfo, 2> buf_infos{};
        buf_infos[0] = {transform_stream_.buffer(), 0, VK_WHOLE_SIZE};
        buf_infos[1] = {property_stream_.buffer(), 0, VK_WHOLE_SIZE};

        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t i = 0; i < 2; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = ds_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buf_infos[i];
        }
        vkUpdateDescriptorSets(device, 2, writes.data(), 0, nullptr);
    }

    VkDescriptorSetLayout InstanceResources::descriptorSetLayout() const noexcept
    {
        return descriptor_svc_->layout(ds_layout_id_);
    }

    // =========================================================================
    //  Buffer accessors
    // =========================================================================

    VkBuffer InstanceResources::transformBuffer() const noexcept { return transform_stream_.buffer(); }
    VkBuffer InstanceResources::prevTransformBuffer() const noexcept { return prev_transform_stream_.buffer(); }
    VkBuffer InstanceResources::propertyBuffer() const noexcept { return property_stream_.buffer(); }
    VkBuffer InstanceResources::cullMetaBuffer() const noexcept { return cull_meta_stream_.buffer(); }
    VkBuffer InstanceResources::meshSectionBuffer() const noexcept { return mesh_section_table_.buffer(); }
    uint32_t InstanceResources::slotCount() const noexcept
    {
        return registry_ ? registry_->slotCount() : slot_count_;
    }

    uint32_t InstanceResources::registerMeshSection(const MeshSectionRecord &section)
    {
        return mesh_section_table_.registerSection(section);
    }

    void InstanceResources::unregisterMeshSection(uint32_t section_id)
    {
        mesh_section_table_.unregisterSection(section_id);
    }

    std::span<const uint32_t> InstanceResources::denseAliveSlots() const noexcept
    {
        return registry_ ? registry_->denseAliveSlots() : std::span<const uint32_t>{};
    }

    std::span<const RenderObjectHandle> InstanceResources::denseAliveHandles() const noexcept
    {
        return registry_ ? registry_->denseAliveHandles() : std::span<const RenderObjectHandle>{};
    }

} // namespace lux::render
