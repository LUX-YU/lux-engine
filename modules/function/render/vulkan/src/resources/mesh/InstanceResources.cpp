#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/renderer/FrameContext.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>
#include <lux/engine/render/resources/mesh/InstanceSlotRegistry.hpp>
#include <lux/engine/render/gpu/lifecycle/VRAMBudgetGuard.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace lux::render
{
    RenderObjectHandle InstanceResources::findSource(ERenderEntityId source) const noexcept
    {
        const auto found = source_objects_.find(source);
        return found == source_objects_.end() ? RenderObjectHandle{} : found->second;
    }

    InstanceResources::ESourceBindResult
    InstanceResources::bindSource(ERenderEntityId source, RenderObjectHandle object)
    {
        if (!isAlive(object))
        {
            return ESourceBindResult::INVALID_OBJECT;
        }
        const auto found = source_objects_.find(source);
        if (found != source_objects_.end())
        {
            return found->second == object ? ESourceBindResult::ALREADY_BOUND : ESourceBindResult::CONFLICT;
        }
        const auto key = (static_cast<std::uint64_t>(object.gen) << 32U) | object.index;
        if (object_sources_.contains(key))
        {
            return ESourceBindResult::CONFLICT;
        }
        source_objects_.emplace(source, object);
        object_sources_.emplace(key, source);
        return ESourceBindResult::INSERTED;
    }

    bool InstanceResources::unbindSource(ERenderEntityId source, RenderObjectHandle expected) noexcept
    {
        const auto found = source_objects_.find(source);
        if (found == source_objects_.end() || found->second != expected)
        {
            return false;
        }
        object_sources_.erase((static_cast<std::uint64_t>(expected.gen) << 32U) | expected.index);
        source_objects_.erase(found);
        return true;
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================
    struct InstanceResources::Backing
    {
        DomainWriteTarget domain;
        std::unique_ptr<SparseInstancePageTable> page_table;
        TSparseInstanceStream<InstanceTransform> transform;
        TSparseInstanceStream<InstanceTransformPrev> previous;
        TSparseInstanceStream<InstanceProperty> property;
        TSparseInstanceStream<InstanceCullMeta> cull;
        TPagedGpuStream<uint32_t> alive;
        TPagedGpuStream<uint32_t> dynamic;
        MeshSectionTable sections;
    };

    InstanceResources::CreateResult InstanceResources::create(const CreateInfo& info) noexcept
    {
        constexpr auto maximum_extent = UINT32_MAX - (kInstanceSlotsPerPage - 1u);
        const bool is_missing_device = !info.device_context.logicalDevice() || !info.device_context.vmaAllocator();
        const bool is_invalid_extent = info.initial_capacity == 0 || info.initial_capacity > info.max_capacity ||
                                       info.max_capacity > maximum_extent;
        const bool is_invalid_page_size = !std::isfinite(info.coordinate_page_size) || info.coordinate_page_size <= 0 ||
                                          info.coordinate_page_size > std::numeric_limits<float>::max() ||
                                          static_cast<float>(info.coordinate_page_size) == 0;
        const bool is_incomplete_domain =
            info.domain_sets.empty() || info.domain_sets.size() > kMaxFramesInFlight ||
            std::ranges::any_of(info.domain_sets, [](VkDescriptorSet set) { return !set; });
        const bool is_invalid_binding = info.domain_binding_offset == UINT32_MAX;
        const bool is_invalid_configuration = is_missing_device || is_invalid_extent || is_invalid_page_size ||
                                              is_incomplete_domain || is_invalid_binding;
        if (is_invalid_configuration)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        DomainWriteTarget domain;
        if (auto accepted = domain.set(info.domain_sets, info.domain_binding_offset); !accepted)
        {
            return lux::cxx::unexpected(accepted.error());
        }
        std::unique_ptr<SparseInstancePageTable> page_table;
        if (info.sparse_bda)
        {
            auto candidate = SparseInstancePageTable::create(info.device_context, info.deferred_queue);
            if (!candidate)
            {
                return lux::cxx::unexpected(candidate.error());
            }
            page_table = std::move(*candidate);
        }
        auto transform = TSparseInstanceStream<InstanceTransform>::create(
            info.device_context,
            info.deferred_queue,
            info.initial_capacity,
            info.sparse_bda
        );
        if (!transform)
        {
            return lux::cxx::unexpected(transform.error());
        }
        auto previous = TSparseInstanceStream<InstanceTransformPrev>::create(
            info.device_context,
            info.deferred_queue,
            info.initial_capacity,
            info.sparse_bda
        );
        if (!previous)
        {
            return lux::cxx::unexpected(previous.error());
        }
        auto property = TSparseInstanceStream<InstanceProperty>::create(
            info.device_context,
            info.deferred_queue,
            info.initial_capacity,
            info.sparse_bda
        );
        if (!property)
        {
            return lux::cxx::unexpected(property.error());
        }
        auto cull = TSparseInstanceStream<InstanceCullMeta>::create(
            info.device_context,
            info.deferred_queue,
            info.initial_capacity,
            info.sparse_bda
        );
        if (!cull)
        {
            return lux::cxx::unexpected(cull.error());
        }
        if (page_table)
        {
            for (uint32_t page = 0; page < transform->pageCount(); ++page)
            {
                const auto accepted = page_table->publish(
                    page,
                    GpuInstancePageAddresses{
                        transform->pageAddress(page),
                        previous->pageAddress(page),
                        property->pageAddress(page),
                        cull->pageAddress(page)
                    }
                );
                if (!accepted)
                {
                    return lux::cxx::unexpected(accepted.error());
                }
            }
        }
        auto alive = TPagedGpuStream<uint32_t>::create(info.device_context, info.deferred_queue, info.max_capacity);
        if (!alive)
        {
            return lux::cxx::unexpected(alive.error());
        }
        auto dynamic = TPagedGpuStream<uint32_t>::create(info.device_context, info.deferred_queue, info.max_capacity);
        if (!dynamic)
        {
            return lux::cxx::unexpected(dynamic.error());
        }
        auto sections = MeshSectionTable::create(info.device_context, info.deferred_queue, info.initial_capacity);
        if (!sections)
        {
            return lux::cxx::unexpected(sections.error());
        }
        Backing backing{
            std::move(domain),
            std::move(page_table),
            std::move(*transform),
            std::move(*previous),
            std::move(*property),
            std::move(*cull),
            std::move(*alive),
            std::move(*dynamic),
            std::move(*sections)
        };
        return std::unique_ptr<InstanceResources>(new InstanceResources(info, std::move(backing)));
    }

    InstanceResources::InstanceResources(const CreateInfo& info, Backing&& backing) noexcept
        : page_table_(std::move(backing.page_table)), transform_stream_(std::move(backing.transform)),
          prev_transform_stream_(std::move(backing.previous)), property_stream_(std::move(backing.property)),
          cull_meta_stream_(std::move(backing.cull)), alive_slot_stream_(std::move(backing.alive)),
          dynamic_slot_stream_(std::move(backing.dynamic)), mesh_section_table_(std::move(backing.sections)),
          registry_(info.initial_capacity), dynamic_positions_(info.initial_capacity, kInvalidDynamicPosition),
          capacity_(info.initial_capacity), max_capacity_(info.max_capacity),
          coordinate_page_size_(static_cast<float>(info.coordinate_page_size)), sparse_bda_(info.sparse_bda),
          domain_(std::move(backing.domain)), device_ctx_(info.device_context), deferred_queue_(info.deferred_queue)
    {
        (void)local_bsphere_.reserve(capacity_);
        refreshDescriptorSet();
    }

    InstanceResources::~InstanceResources() noexcept
    {
        // Runtime-lazy MDC backing can still be referenced by compiled frames.
        for (uint32_t i = 0; i < kMdcInfoRingSize; ++i)
        {
            if (mdc_info_buffers_[i])
            {
                deferred_queue_.retireBuffer(mdc_info_buffers_[i], mdc_info_allocs_[i]);
            }
        }
    }

    // =========================================================================
    //  MDC info upload
    // =========================================================================

    void InstanceResources::uploadMdcInfo()
    {
        mdc_table_.buildOffsets();
        const auto& gpu_data = mdc_table_.gpuData();
        assert(!gpu_data.empty() && "buildOffsets must always produce at least a safety sentinel");

        const VkDeviceSize required = static_cast<VkDeviceSize>(gpu_data.size()) * sizeof(uint32_t);

        // Advance the ring once per graph compile. Multiple cull passes in the
        // SAME compile (Forward + Deferred, multiple views) call uploadMdcInfo
        // with the same frame serial and must share one slot; the next compile
        // (new serial) takes a fresh slot so in-flight frames keep reading the
        // slot they were recorded with.
        if (mdc_info_current_serial_ != mdc_info_last_upload_serial_)
        {
            mdc_info_ring_cursor_ = (mdc_info_ring_cursor_ + 1u) % kMdcInfoRingSize;
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
                deferred_queue_.retireBuffer(mdc_info_buffers_[slot], mdc_info_allocs_[slot]);
            }

            mdc_info_buffers_[slot] = VK_NULL_HANDLE;
            mdc_info_allocs_[slot] = nullptr;
            mdc_info_mapped_[slot] = nullptr;
            mdc_info_sizes_[slot] = 0;

            VkBuffer buf{VK_NULL_HANDLE};
            VmaAllocation alloc{nullptr};
            void* mapped{nullptr};
            const auto allocation_result = createGpuBufferVmaBuffer(
                device_ctx_.vmaAllocator(),
                required,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                /*cpu_writable=*/true,
                &buf,
                &alloc,
                &mapped
            );

            // On allocation failure leave the slot null instead of publishing a
            // half-built (already-retired) handle to the render graph.
            const bool allocation_failed =
                allocation_result != VK_SUCCESS || buf == VK_NULL_HANDLE || mapped == nullptr;
            if (allocation_failed)
            {
                if (buf != VK_NULL_HANDLE)
                    vmaDestroyBuffer(device_ctx_.vmaAllocator(), buf, alloc);
                return;
            }

            mdc_info_buffers_[slot] = buf;
            mdc_info_allocs_[slot] = alloc;
            mdc_info_mapped_[slot] = mapped;
            mdc_info_sizes_[slot] = required;
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
        if (registry_.needsGrowForAllocate())
        {
            const uint32_t required = registry_.slotCount() + 1u;
            if (!ensureCapacity(required))
                return InstanceSlot::invalid();
        }

        const InstanceSlot slot = registry_.allocate();
        if (!slot)
            return InstanceSlot::invalid();

        slot_count_ = registry_.slotCount();
        appendAliveSlot();

        auto& prop = property_stream_.at(slot.index);
        auto& cull = cull_meta_stream_.at(slot.index);
        cull.bsphere[0] = 0.0f;
        cull.bsphere[1] = 0.0f;
        cull.bsphere[2] = 0.0f;
        cull.bsphere[3] = 0.0f;
        std::fill(std::begin(cull.bsphere_page), std::end(cull.bsphere_page), 0);
        cull.lod_count = 0u;
        prop.transform_index = slot.index;
        prop.object_id = kInvalidObjectId;
        prop.flags = 0u;
        prop.pass_and_geometry =
            static_cast<uint32_t>(kPassMaskOpaqueDefault) | (static_cast<uint32_t>(EGeometryKind::STATIC_MESH) << 16u);
        local_bsphere_.at(slot.index) = {0.f, 0.f, 0.f, 0.f};
        addDynamicSlot(slot.index);

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
        if (!registry_.isAlive(slot))
            return;

        const auto handle = handleForSlot(slot);
        const auto source = object_sources_.find((static_cast<std::uint64_t>(handle.gen) << 32U) | handle.index);
        if (source != object_sources_.end())
        {
            (void)unbindSource(source->second, handle);
        }
        auto& cull = cull_meta_stream_.at(slot.index);
        unregisterInstanceLods(cull);
        removeDynamicSlot(slot.index);

        const uint32_t dense_position = registry_.densePosition(slot);
        if (!registry_.free(slot))
            return;

        slot_count_ = registry_.slotCount();
        repairAliveSlotAfterFree(dense_position);

        // 逐位计数销账:slot 回收后其 flags 不再计入存活位。清零而非仅
        // 记账,避免 allocate 复用残留旧位导致重复计数。标脏保持 CPU/GPU
        // 镜像一致(墓碑槽虽不被绘制,但不留隐式的"下个写者恰好标脏"链)。
        auto& prop = property_stream_.at(slot.index);
        accountFlagsDiff(prop.flags, 0u);
        prop.flags = 0u;
        markPropertyDirty(slot);

        // Tombstone convention: bsphere.w < 0.
        cull.bsphere[3] = -1.0f;
        markCullDirty(slot);
    }

    bool InstanceResources::isAlive(InstanceSlot slot) const noexcept
    {
        return registry_.isAlive(slot);
    }

    RenderObjectHandle InstanceResources::allocateObject()
    {
        if (registry_.needsGrowForAllocate())
        {
            const uint32_t required = registry_.slotCount() + 1u;
            if (!ensureCapacity(required))
                return RenderObjectHandle::invalid();
        }

        const RenderObjectHandle handle = registry_.allocateObject();
        if (!handle)
            return RenderObjectHandle::invalid();

        const InstanceSlot slot = registry_.resolveSlot(handle);
        if (!slot)
            return RenderObjectHandle::invalid();

        slot_count_ = registry_.slotCount();
        appendAliveSlot();

        auto& prop = property_stream_.at(slot.index);
        auto& cull = cull_meta_stream_.at(slot.index);
        cull.bsphere[0] = 0.0f;
        cull.bsphere[1] = 0.0f;
        cull.bsphere[2] = 0.0f;
        cull.bsphere[3] = 0.0f;
        std::fill(std::begin(cull.bsphere_page), std::end(cull.bsphere_page), 0);
        cull.lod_count = 0u;
        prop.object_id = handle.index;
        prop.transform_index = slot.index;
        prop.flags = 0u;
        prop.pass_and_geometry =
            static_cast<uint32_t>(kPassMaskOpaqueDefault) | (static_cast<uint32_t>(EGeometryKind::STATIC_MESH) << 16u);
        local_bsphere_.at(slot.index) = {0.f, 0.f, 0.f, 0.f};
        addDynamicSlot(slot.index);

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

    namespace
    {
        [[nodiscard]] constexpr std::uint64_t objectKey(RenderObjectHandle object) noexcept
        {
            return static_cast<std::uint64_t>(object.gen) << 32u | object.index;
        }
    }

    void InstanceResources::setSubmission(RenderObjectHandle object, RenderSubmissionState state) noexcept
    {
        const auto found = resource_bindings_.find(objectKey(object));
        if (found != resource_bindings_.end())
            found->second.submission = std::move(state);
    }

    void InstanceResources::retainSubmissions(const FrameRuntime& frame) const noexcept
    {
        for (const auto& [_, binding] : resource_bindings_)
            frame.retainSubmission(binding.submission);
    }

    bool InstanceResources::bindResources(RenderObjectHandle object, ResourceBinding binding)
    {
        if (!isAlive(object) || !binding.mesh.isValid() || !binding.material.isValid())
        {
            return false;
        }
        return resource_bindings_.emplace(objectKey(object), binding).second;
    }

    std::optional<InstanceResources::ResourceBinding> InstanceResources::replaceResources(
        RenderObjectHandle object,
        ResourceBinding binding
    ) noexcept
    {
        if (!isAlive(object) || !binding.mesh.isValid() || !binding.material.isValid())
        {
            return std::nullopt;
        }
        const auto found = resource_bindings_.find(objectKey(object));
        if (found == resource_bindings_.end())
            return std::nullopt;
        const auto previous = found->second;
        found->second = binding;
        return previous;
    }

    std::optional<InstanceResources::ResourceBinding> InstanceResources::resourceBinding(RenderObjectHandle object
    ) const noexcept
    {
        if (!isAlive(object))
            return std::nullopt;
        const auto found = resource_bindings_.find(objectKey(object));
        return found == resource_bindings_.end() ? std::nullopt : std::optional<ResourceBinding>{found->second};
    }

    std::optional<InstanceResources::ResourceBinding> InstanceResources::takeResources(RenderObjectHandle object
    ) noexcept
    {
        const auto found = resource_bindings_.find(objectKey(object));
        if (found == resource_bindings_.end())
            return std::nullopt;
        const auto result = found->second;
        resource_bindings_.erase(found);
        return result;
    }

    std::vector<InstanceResources::ResourceBinding> InstanceResources::takeAllResources()
    {
        std::vector<ResourceBinding> result;
        result.reserve(resource_bindings_.size());
        for (const auto& [_, binding] : resource_bindings_)
            result.push_back(binding);
        source_objects_.clear();
        object_sources_.clear();
        resource_bindings_.clear();
        fade_retirements_.clear();
        return result;
    }

    bool InstanceResources::beginFadeRetirement(
        RenderObjectHandle object,
        float scene_time,
        float duration_seconds,
        std::uint32_t transition_seed
    ) noexcept
    {
        const auto slot = resolveSlot(object);
        const bool is_invalid_slot = !isAlive(slot);
        const bool is_invalid_time =
            !std::isfinite(scene_time) || !std::isfinite(duration_seconds) || duration_seconds <= 0.0f;
        const bool is_invalid_transition = transition_seed == 0u;
        const bool is_invalid_request = is_invalid_slot || is_invalid_time || is_invalid_transition;
        if (is_invalid_request)
        {
            return false;
        }
        auto& property = propertyAt(slot);
        const auto pass_mask = static_cast<PassMask>(property.pass_and_geometry & 0xffffu);
        if (hasPass(pass_mask, EPassBit::PASS_TRANSPARENT))
        {
            ++transparent_hard_cut_count_;
            return false;
        }
        if (std::ranges::any_of(fade_retirements_, [object](const FadeRetirement& retirement) {
                return retirement.object == object;
            }))
        {
            return true;
        }
        property.transition_start_time = scene_time;
        property.transition_duration = duration_seconds;
        property.transition_seed = transition_seed;
        property.transition_flags = 3u;
        markPropertyDirty(slot);
        fade_retirements_.push_back({object, scene_time + duration_seconds});
        return true;
    }

    std::vector<RenderObjectHandle> InstanceResources::collectExpiredFadeRetirements(float scene_time)
    {
        std::vector<RenderObjectHandle> result;
        if (!std::isfinite(scene_time))
            return result;
        for (auto iterator = fade_retirements_.begin(); iterator != fade_retirements_.end();)
        {
            if (!isAlive(iterator->object) || scene_time >= iterator->expires_at)
            {
                if (isAlive(iterator->object))
                    result.push_back(iterator->object);
                iterator = fade_retirements_.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        return result;
    }

    void InstanceResources::cancelFadeRetirement(RenderObjectHandle object) noexcept
    {
        std::erase_if(fade_retirements_, [object](const FadeRetirement& retirement) {
            return retirement.object == object;
        });
    }

    bool InstanceResources::isAlive(RenderObjectHandle handle) const noexcept
    {
        return registry_.isAlive(handle);
    }

    uint32_t InstanceResources::generation(InstanceSlot slot) const noexcept
    {
        return registry_.generation(slot);
    }

    InstanceSlot InstanceResources::resolveSlot(RenderObjectHandle handle) const noexcept
    {
        return registry_.resolveSlot(handle);
    }

    RenderObjectHandle InstanceResources::handleForSlot(InstanceSlot slot) const noexcept
    {
        return registry_.handleForSlot(slot);
    }

    bool InstanceResources::ensureCapacity(uint32_t required)
    {
        if (required <= capacity_)
            return true;

        if (required > max_capacity_)
            return false;

        const auto next_page_capacity =
            static_cast<std::uint64_t>((required - 1u) / kInstanceSlotsPerPage + 1u) * kInstanceSlotsPerPage;
        const auto new_cap = static_cast<std::uint32_t>(std::min<std::uint64_t>(next_page_capacity, max_capacity_));
        if (new_cap < required)
            return false;

        const auto old_page_count = transform_stream_.pageCount();
        const auto new_page_count = (new_cap - 1u) / kInstanceSlotsPerPage + 1u;

        // Sparse growth preserves accepted pages; flat growth prepares full replacements while
        // the previous buffers remain alive. Budget the actual candidate allocation in each mode.
        if (new_page_count > old_page_count)
        {
            const auto allocated_pages = sparse_bda_ ? new_page_count - old_page_count : new_page_count;
            const VkDeviceSize bytes_needed = VkDeviceSize(allocated_pages) * kInstanceSlotsPerPage *
                                              (sizeof(InstanceTransform) + sizeof(InstanceTransformPrev) +
                                               sizeof(InstanceProperty) + sizeof(InstanceCullMeta));
            VRAMBudgetGuard budget(device_ctx_.vmaAllocator());
            if (!budget.canAllocate(bytes_needed))
                return false;
        }

        // No field becomes visible or retires old backing until every candidate and the
        // sparse page-table entry are ready. There are no callbacks or waits before commit.
        auto transform = transform_stream_.prepareGrowth(new_cap);
        if (!transform)
        {
            return false;
        }
        auto previous = prev_transform_stream_.prepareGrowth(new_cap);
        if (!previous)
        {
            return false;
        }
        auto property = property_stream_.prepareGrowth(new_cap);
        if (!property)
        {
            return false;
        }
        auto cull = cull_meta_stream_.prepareGrowth(new_cap);
        if (!cull)
        {
            return false;
        }

        if (sparse_bda_ && new_page_count > old_page_count)
        {
            // Allocation admits one slot at a time, so growth crosses at most one physical page.
            const auto page_index = new_page_count - 1u;
            const auto accepted = page_table_->publish(
                page_index,
                GpuInstancePageAddresses{
                    .transform = transform_stream_.pageAddress(*transform, page_index),
                    .previous_transform = prev_transform_stream_.pageAddress(*previous, page_index),
                    .property = property_stream_.pageAddress(*property, page_index),
                    .cull_meta = cull_meta_stream_.pageAddress(*cull, page_index),
                }
            );
            if (!accepted)
            {
                return false;
            }
        }

        transform_stream_.commitGrowth(std::move(*transform));
        prev_transform_stream_.commitGrowth(std::move(*previous));
        property_stream_.commitGrowth(std::move(*property));
        cull_meta_stream_.commitGrowth(std::move(*cull));
        (void)local_bsphere_.reserve(new_cap);
        dynamic_positions_.resize(new_cap, kInvalidDynamicPosition);
        registry_.resizeCapacity(new_cap);

        capacity_ = new_cap;
        slot_count_ = registry_.slotCount();
        if (!sparse_bda_)
        {
            full_rebuild_ = true;
            refreshDescriptorSet();
        }
        return true;
    }

    // =========================================================================
    //  Per-stream writes
    // =========================================================================

    void InstanceResources::writeTransform(InstanceSlot slot, const InstanceTransform& xform)
    {
        prev_transform_stream_.at(slot.index) = transform_stream_.at(slot.index);
        markPrevTransformDirty(slot);
        transform_stream_.at(slot.index) = xform;
        markTransformDirty(slot);
        recomputeWorldBsphere(slot);
    }

    void InstanceResources::writePrevTransform(InstanceSlot slot, const InstanceTransformPrev& xform_prev)
    {
        prev_transform_stream_.at(slot.index) = xform_prev;
        markPrevTransformDirty(slot);
    }

    void InstanceResources::setLocalBsphere(InstanceSlot slot, float cx, float cy, float cz, float r)
    {
        local_bsphere_.at(slot.index) = {cx, cy, cz, r};
        recomputeWorldBsphere(slot);
    }

    void InstanceResources::recomputeWorldBsphere(InstanceSlot slot)
    {
        const auto& lb = local_bsphere_.at(slot.index);
        const auto& transform = transform_stream_.at(slot.index);
        const float* M = transform.basis_local; // three basis columns, local xyz in w

        // Transform the local center, but retain the result as page + normalized
        // page-local float.  Collapsing this to one scene-relative float made all
        // view/HZB/shadow culling lose precision again at large coordinates.
        float cx = lb[0], cy = lb[1], cz = lb[2], r = lb[3];
        const double local[3]{
            static_cast<double>(M[0] * cx + M[4] * cy + M[8] * cz + M[3]),
            static_cast<double>(M[1] * cx + M[5] * cy + M[9] * cz + M[7]),
            static_cast<double>(M[2] * cx + M[6] * cy + M[10] * cz + M[11])
        };

        // Conservative radius: scale by max column norm of upper-left 3x3.
        // Use squared norms + single sqrtf on the max to avoid 3 sqrt calls.
        float sq0 = M[0] * M[0] + M[1] * M[1] + M[2] * M[2];
        float sq1 = M[4] * M[4] + M[5] * M[5] + M[6] * M[6];
        float sq2 = M[8] * M[8] + M[9] * M[9] + M[10] * M[10];
        float max_scale = std::sqrt(std::max({sq0, sq1, sq2}));

        auto& meta = cull_meta_stream_.at(slot.index);
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const double carry_value = std::floor(local[axis] / coordinate_page_size_);
            const auto carry = static_cast<std::int64_t>(carry_value);
            const auto page = static_cast<std::int64_t>(transform.page_delta[axis]) + carry;
            if (page < std::numeric_limits<std::int32_t>::min() || page > std::numeric_limits<std::int32_t>::max())
                renderFatal("Instance cull sphere exceeds RenderScene int32 page range");

            meta.bsphere_page[axis] = static_cast<std::int32_t>(page);
            meta.bsphere[axis] = static_cast<float>(local[axis] - static_cast<double>(carry) * coordinate_page_size_);
        }
        meta.bsphere_page[3] = 0;
        meta.bsphere[3] = r * max_scale;
        markCullDirty(slot);
    }

    void InstanceResources::accountFlagsDiff(uint32_t old_flags, uint32_t new_flags) noexcept
    {
        uint32_t changed = old_flags ^ new_flags;
        while (changed)
        {
            const uint32_t bit = static_cast<uint32_t>(std::countr_zero(changed));
            changed &= changed - 1u;
            if (new_flags & (1u << bit))
            {
                ++flag_counts_[bit];
            }
            else
            {
                // 穿底 = 有写点绕过了 setInstanceFlags/writeProperty/free 的
                // 收口直改 flags(漏账)。静默钳制会把 bug 藏成"高亮偶尔
                // 不消失"这类极难查的表象——debug 断言,release 钳制兜底。
                assert(flag_counts_[bit] > 0u && "flag census underflow: flags written outside the accounted APIs");
                if (flag_counts_[bit] > 0u)
                    --flag_counts_[bit];
            }
        }
    }

    void InstanceResources::setInstanceFlags(InstanceSlot slot, uint32_t flags)
    {
        auto& prop = property_stream_.at(slot.index);
        updateDynamicMembership(slot.index, prop.flags, flags);
        accountFlagsDiff(prop.flags, flags);
        prop.flags = flags;
        markPropertyDirty(slot);
    }

    void InstanceResources::writeProperty(InstanceSlot slot, const InstanceProperty& prop)
    {
        updateDynamicMembership(slot.index, property_stream_.at(slot.index).flags, prop.flags);
        accountFlagsDiff(property_stream_.at(slot.index).flags, prop.flags);
        property_stream_.at(slot.index) = prop;
        markPropertyDirty(slot);
    }

    void InstanceResources::writeCullMeta(InstanceSlot slot, const InstanceCullMeta& meta)
    {
        auto& current = cull_meta_stream_.at(slot.index);
        // Overwriting a live registration → release the previous LODs' MDCs +
        // sections first (the new ones were already registered by the caller).
        if (current.lod_count != 0u)
            unregisterInstanceLods(current);

        current = meta;
        // World-space bsphere is derived data from local_bsphere_ + transform.
        // Recompute here so caller write order cannot accidentally zero it out.
        recomputeWorldBsphere(slot);
    }

    InstanceTransform& InstanceResources::transformAt(InstanceSlot slot) noexcept
    {
        return transform_stream_.at(slot.index);
    }

    InstanceTransformPrev& InstanceResources::prevTransformAt(InstanceSlot slot) noexcept
    {
        return prev_transform_stream_.at(slot.index);
    }

    InstanceProperty& InstanceResources::propertyAt(InstanceSlot slot) noexcept
    {
        return property_stream_.at(slot.index);
    }

    const InstanceProperty& InstanceResources::propertyAt(InstanceSlot slot) const noexcept
    {
        return property_stream_.at(slot.index);
    }

    InstanceCullMeta& InstanceResources::cullMetaAt(InstanceSlot slot) noexcept
    {
        return cull_meta_stream_.at(slot.index);
    }

    const InstanceCullMeta& InstanceResources::cullMetaAt(InstanceSlot slot) const noexcept
    {
        return cull_meta_stream_.at(slot.index);
    }

    bool InstanceResources::canRebaseSceneOrigin(const std::int64_t origin_delta[3]) const noexcept
    {
        for (const auto slot_index : registry_.denseAliveSlots())
        {
            const auto& current = transform_stream_.at(slot_index);
            const auto& previous = prev_transform_stream_.at(slot_index);
            if (!canRebaseRenderPageDelta(current.page_delta, origin_delta) ||
                !canRebaseRenderPageDelta(previous.page_delta, origin_delta))
            {
                return false;
            }
        }
        return true;
    }

    void InstanceResources::rebaseSceneOrigin(const std::int64_t origin_delta[3]) noexcept
    {
        for (const auto slot_index : registry_.denseAliveSlots())
        {
            const InstanceSlot slot{slot_index};
            auto& current = transform_stream_.at(slot_index);
            auto& previous = prev_transform_stream_.at(slot_index);
            rebaseRenderPageDelta(current.page_delta, origin_delta);
            rebaseRenderPageDelta(previous.page_delta, origin_delta);
            markTransformDirty(slot);
            markPrevTransformDirty(slot);
            recomputeWorldBsphere(slot);
        }
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
        auto& prop = property_stream_.at(slot.index);
        prop.pass_and_geometry = static_cast<uint32_t>(pass_mask) | (static_cast<uint32_t>(geometry_kind) << 16u);
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

    void InstanceResources::appendAliveSlot()
    {
        const auto alive = registry_.denseAliveSlots();
        if (alive.empty())
            return;
        const uint32_t dense_position = static_cast<uint32_t>(alive.size() - 1u);
        alive_slot_stream_.at(dense_position) = alive.back();
        alive_slot_stream_.markDirty(dense_position);
    }

    void InstanceResources::repairAliveSlotAfterFree(uint32_t dense_position)
    {
        const auto alive = registry_.denseAliveSlots();
        if (dense_position == InstanceSlotRegistry::kInvalidDensePos || dense_position >= alive.size())
        {
            return;
        }
        alive_slot_stream_.at(dense_position) = alive[dense_position];
        alive_slot_stream_.markDirty(dense_position);
    }

    void InstanceResources::rebuildAliveSlotStream()
    {
        const auto alive = registry_.denseAliveSlots();
        for (uint32_t dense_position = 0u; dense_position < alive.size(); ++dense_position)
            alive_slot_stream_.at(dense_position) = alive[dense_position];
    }

    void InstanceResources::addDynamicSlot(std::uint32_t slot_index)
    {
        if (slot_index >= dynamic_positions_.size() || dynamic_positions_[slot_index] != kInvalidDynamicPosition)
        {
            return;
        }
        const auto position = static_cast<std::uint32_t>(dense_dynamic_slots_.size());
        dense_dynamic_slots_.push_back(slot_index);
        dynamic_positions_[slot_index] = position;
        dynamic_slot_stream_.at(position) = slot_index;
        dynamic_slot_stream_.markDirty(position);
    }

    void InstanceResources::removeDynamicSlot(std::uint32_t slot_index)
    {
        if (slot_index >= dynamic_positions_.size())
            return;
        const auto position = dynamic_positions_[slot_index];
        if (position == kInvalidDynamicPosition)
            return;

        const auto last_slot = dense_dynamic_slots_.back();
        dense_dynamic_slots_[position] = last_slot;
        dense_dynamic_slots_.pop_back();
        dynamic_positions_[slot_index] = kInvalidDynamicPosition;
        if (position < dense_dynamic_slots_.size())
        {
            dynamic_positions_[last_slot] = position;
            dynamic_slot_stream_.at(position) = last_slot;
            dynamic_slot_stream_.markDirty(position);
        }
    }

    void InstanceResources::updateDynamicMembership(
        std::uint32_t slot_index,
        std::uint32_t old_flags,
        std::uint32_t new_flags
    )
    {
        const bool was_dynamic = (old_flags & kInstanceInternalFlagClusterOwned) == 0u;
        const bool is_dynamic = (new_flags & kInstanceInternalFlagClusterOwned) == 0u;
        if (was_dynamic == is_dynamic)
            return;
        if (is_dynamic)
            addDynamicSlot(slot_index);
        else
            removeDynamicSlot(slot_index);
    }

    void InstanceResources::rebuildDynamicSlotStream()
    {
        dense_dynamic_slots_.clear();
        std::fill(dynamic_positions_.begin(), dynamic_positions_.end(), kInvalidDynamicPosition);
        for (const auto slot_index : registry_.denseAliveSlots())
        {
            if ((property_stream_.at(slot_index).flags & kInstanceInternalFlagClusterOwned) == 0u)
            {
                addDynamicSlot(slot_index);
            }
        }
    }

    // =========================================================================\n//  Transfer Scheduler path\n//
    // =========================================================================

    void InstanceResources::submitTransfers(TransferScheduler& scheduler)
    {
        if (slot_count_ == 0)
            return;

        const bool has_work = full_rebuild_ || transform_stream_.hasDirtyPages() ||
                              prev_transform_stream_.hasDirtyPages() || property_stream_.hasDirtyPages() ||
                              cull_meta_stream_.hasDirtyPages() || alive_slot_stream_.hasDirtyPages() ||
                              dynamic_slot_stream_.hasDirtyPages() || mesh_section_table_.hasWork();
        if (!has_work)
            return;

        const bool full_upload = full_rebuild_;
        const bool upload_transform = full_upload || transform_stream_.hasDirtyPages();
        const bool upload_prev_transform = full_upload || prev_transform_stream_.hasDirtyPages();
        const bool upload_property = full_upload || property_stream_.hasDirtyPages();
        const bool upload_cull = full_upload || cull_meta_stream_.hasDirtyPages();
        const bool upload_alive_slots = full_upload || alive_slot_stream_.hasDirtyPages();
        const bool upload_dynamic_slots = full_upload || dynamic_slot_stream_.hasDirtyPages();
        const bool upload_section = full_upload || mesh_section_table_.hasWork();

        // ── Collect chunks from all dirty instance streams ──────────────
        // Reuse the member scratch (cleared here) instead of fresh per-tick
        // vectors. collectUploadChunks only appends, so clear() before each. (P-5)
        xform_chunks_.clear();
        prev_xform_chunks_.clear();
        prop_chunks_.clear();
        cull_chunks_.clear();
        alive_slot_chunks_.clear();
        dynamic_slot_chunks_.clear();

        const VkDeviceSize xform_bytes =
            upload_transform ? transform_stream_.collectUploadChunks(slot_count_, full_upload, xform_chunks_) : 0u;
        const VkDeviceSize prev_xform_bytes =
            upload_prev_transform
                ? prev_transform_stream_.collectUploadChunks(slot_count_, full_upload, prev_xform_chunks_)
                : 0u;
        const VkDeviceSize prop_bytes =
            upload_property ? property_stream_.collectUploadChunks(slot_count_, full_upload, prop_chunks_) : 0u;
        const VkDeviceSize cull_bytes =
            upload_cull ? cull_meta_stream_.collectUploadChunks(slot_count_, full_upload, cull_chunks_) : 0u;
        const uint32_t alive_count = aliveCount();
        const VkDeviceSize alive_slot_bytes =
            (upload_alive_slots && alive_slot_stream_.buffer() != VK_NULL_HANDLE)
                ? alive_slot_stream_.collectUploadChunks(alive_count, full_upload, alive_slot_chunks_)
                : 0u;
        const uint32_t dynamic_count = dynamicCount();
        const VkDeviceSize dynamic_slot_bytes =
            (upload_dynamic_slots && dynamic_slot_stream_.buffer() != VK_NULL_HANDLE)
                ? dynamic_slot_stream_.collectUploadChunks(dynamic_count, full_upload, dynamic_slot_chunks_)
                : 0u;

        const VkDeviceSize total_instance_bytes =
            xform_bytes + prev_xform_bytes + prop_bytes + cull_bytes + alive_slot_bytes + dynamic_slot_bytes;

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
                auto* dst = static_cast<uint8_t*>(stg.mapped);
                VkDeviceSize staging_offset = 0u;

                auto emitPagedStreamCopies = [&](const auto& chunks, EBufferDomain domain) {
                    for (const auto& chunk : chunks)
                    {
                        std::memcpy(dst + staging_offset, chunk.src, static_cast<size_t>(chunk.size));
                        scheduler.submitBufferCopy({
                            .src = stg.buffer,
                            .src_offset = stg.srcOffset + staging_offset,
                            .dst = chunk.destination,
                            .dst_offset = chunk.destination_offset,
                            .size = chunk.size,
                            .domain = domain,
                        });
                        staging_offset += chunk.size;
                    }
                };
                auto emitFlatStreamCopies = [&](VkBuffer gpu_buf, const auto& chunks, EBufferDomain domain) {
                    if (gpu_buf == VK_NULL_HANDLE)
                        return;
                    for (const auto& chunk : chunks)
                    {
                        std::memcpy(dst + staging_offset, chunk.src, static_cast<size_t>(chunk.size));
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

                emitPagedStreamCopies(xform_chunks_, EBufferDomain::STORAGE_VS);
                emitPagedStreamCopies(prev_xform_chunks_, EBufferDomain::STORAGE_VS);
                emitPagedStreamCopies(prop_chunks_, EBufferDomain::STORAGE_ALL);
                emitPagedStreamCopies(cull_chunks_, EBufferDomain::STORAGE_CS);
                emitFlatStreamCopies(alive_slot_stream_.buffer(), alive_slot_chunks_, EBufferDomain::STORAGE_CS);
                emitFlatStreamCopies(dynamic_slot_stream_.buffer(), dynamic_slot_chunks_, EBufferDomain::STORAGE_CS);
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
            if (upload_transform)
                transform_stream_.clearDirtyState();
            if (upload_prev_transform)
                prev_transform_stream_.clearDirtyState();
            if (upload_property)
                property_stream_.clearDirtyState();
            if (upload_cull)
                cull_meta_stream_.clearDirtyState();
            if (upload_alive_slots && alive_slot_stream_.buffer() != VK_NULL_HANDLE)
            {
                alive_slot_stream_.clearDirtyState();
            }
            if (upload_dynamic_slots && dynamic_slot_stream_.buffer() != VK_NULL_HANDLE)
            {
                dynamic_slot_stream_.clearDirtyState();
            }
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
        VkDevice device = device_ctx_.logicalDevice();

        std::array<VkDescriptorBufferInfo, 2> buf_infos{};
        const auto transform_buffer = sparse_bda_ ? page_table_->rootBuffer() : transform_stream_.buffer();
        const auto property_buffer = sparse_bda_ ? page_table_->rootBuffer() : property_stream_.buffer();
        buf_infos[0] = {transform_buffer, 0, VK_WHOLE_SIZE};
        buf_infos[1] = {property_buffer, 0, VK_WHOLE_SIZE};

        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t i = 0; i < 2; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buf_infos[i];
        }

        // 阶段 C:域集是唯一写目标(legacy per-set 半边已删)。
        //
        // 本资源只有一个集(不按 frames-in-flight 分),而域集是逐 slice 的,
        // 所以每个 slice 都要写 —— 漏掉一个 slice 会让那一帧读到陈旧缓冲,
        // 且只在特定帧序下才显形。
        for (uint32_t s = 0; s < domain_.sliceCount(); ++s)
        {
            VkDescriptorSet domain_ds = domain_.setFor(s);
            for (uint32_t i = 0; i < 2; ++i)
            {
                writes[i].dstSet = domain_ds;
                writes[i].dstBinding = domain_.binding(i);
            }
            vkUpdateDescriptorSets(device, 2, writes.data(), 0, nullptr);
            descriptor_write_count_ += 2u;
        }
    }

    // =========================================================================
    //  Buffer accessors
    // =========================================================================

    VkBuffer InstanceResources::transformBuffer() const noexcept
    {
        return sparse_bda_ ? page_table_->rootBuffer() : transform_stream_.buffer();
    }

    VkBuffer InstanceResources::prevTransformBuffer() const noexcept
    {
        return sparse_bda_ ? page_table_->rootBuffer() : prev_transform_stream_.buffer();
    }

    VkBuffer InstanceResources::propertyBuffer() const noexcept
    {
        return sparse_bda_ ? page_table_->rootBuffer() : property_stream_.buffer();
    }

    VkBuffer InstanceResources::cullMetaBuffer() const noexcept
    {
        return sparse_bda_ ? page_table_->rootBuffer() : cull_meta_stream_.buffer();
    }

    VkBuffer InstanceResources::aliveSlotBuffer() const noexcept
    {
        return alive_slot_stream_.buffer();
    }

    VkBuffer InstanceResources::dynamicSlotBuffer() const noexcept
    {
        return dynamic_slot_stream_.buffer();
    }

    VkBuffer InstanceResources::meshSectionBuffer() const noexcept
    {
        return mesh_section_table_.buffer();
    }
    uint32_t InstanceResources::slotCount() const noexcept
    {
        return registry_.slotCount();
    }

    uint32_t InstanceResources::registerMeshSection(
        const MeshSectionRecord& section,
        uint16_t ibo_segment,
        VkIndexType index_type
    )
    {
        return mesh_section_table_.registerSection(section, ibo_segment, index_type);
    }

    void InstanceResources::unregisterMeshSection(uint32_t section_id)
    {
        mesh_section_table_.unregisterSection(section_id);
    }

    std::span<const uint32_t> InstanceResources::denseAliveSlots() const noexcept
    {
        return registry_.denseAliveSlots();
    }

} // namespace lux::render
