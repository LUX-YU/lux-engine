#include <atomic>
#include <algorithm>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/ShaderBindingPlan.hpp>
#include <unordered_set>
#include <lux/engine/render/execution/FrameResources.hpp>

namespace lux::render::detail
{
    namespace
    {
        constexpr auto descriptor_counts = std::array{
            &nri::DescriptorPoolDesc::descriptorSetMaxNum, &nri::DescriptorPoolDesc::samplerMaxNum,
            &nri::DescriptorPoolDesc::constantBufferMaxNum, &nri::DescriptorPoolDesc::storageStructuredBufferMaxNum,
            &nri::DescriptorPoolDesc::storageBufferMaxNum, &nri::DescriptorPoolDesc::textureMaxNum,
            &nri::DescriptorPoolDesc::storageTextureMaxNum
        };
    }

    RenderResult<void>
    RenderGraphInstance::FrameResources::addDescriptors(
        nri::DescriptorPoolDesc& pool,
        const PassBindings& program
    ) noexcept
    {
        if (program.private_bindings.empty()) return {};
        if (pool.descriptorSetMaxNum == UINT32_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
        ++pool.descriptorSetMaxNum;
        for (const auto& binding : program.private_bindings)
        {
            auto* count = [&]() noexcept -> std::uint32_t*
            {
                switch (binding.kind)
                {
                case EDescriptorKind::TYPED_STORAGE_BUFFER: return &pool.storageBufferMaxNum;
                case EDescriptorKind::STORAGE_BUFFER: return &pool.storageStructuredBufferMaxNum;
                case EDescriptorKind::UNIFORM_BUFFER: return &pool.constantBufferMaxNum;
                case EDescriptorKind::STORAGE_IMAGE: return &pool.storageTextureMaxNum;
                case EDescriptorKind::SAMPLED_IMAGE: return &pool.textureMaxNum;
                case EDescriptorKind::SAMPLER: return &pool.samplerMaxNum;
                }
                return nullptr;
            }();
            if (!count || binding.count > UINT32_MAX - *count)
                return cxx::unexpected(RenderError{kDeviceCapacity});
            *count += binding.count;
        }
        return {};
    }

    RenderResult<nri::DescriptorSet*>
    RenderGraphInstance::prepareRasterBindings(
        std::uint32_t slot,
        const PassBindings& bindings,
        std::uint64_t binding_key,
        std::span<const ResourceInput> inputs
    ) noexcept
    {
        auto& frame = frames_[slot];
        auto& indices = frame.raster_binding_indices[&bindings];
        auto found = indices.find(binding_key);
        if (found == indices.end())
        {
            const auto index = static_cast<std::uint32_t>(frame.raster_bindings.size());
            frame.raster_bindings.push_back({});
            auto& record = frame.raster_bindings.back();
            record.binding_key = binding_key;
            record.bindings = &bindings;
            record.views.resize(bindings.uses.size());
            record.inputs.resize(bindings.uses.size());
            found = indices.emplace(binding_key, index).first;
        }
        auto& record = frame.raster_bindings[found->second];
        const auto& core = session_->nriCore();
        if (!record.set)
        {
            nri::DescriptorPoolDesc additional{};
            if (auto counted = FrameResources::addDescriptors(additional, bindings); !counted)
                return cxx::unexpected(counted.error());
            const bool fits = frame.descriptor_pool && std::ranges::all_of(descriptor_counts, [&](auto field) noexcept
            {
                return additional.*field <= frame.descriptor_capacity.*field - frame.descriptor_reserved.*field;
            });
            if (fits)
            {
                // NRI does not promise to restore pool quota on failed allocation (its validation
                // backend consumes range counters before the native call). Reserve this attempt
                // until the pool is rebuilt; never reuse uncertain quota or discard prior live sets.
                for (auto field : descriptor_counts) frame.descriptor_reserved.*field += additional.*field;
                const auto status = core.AllocateDescriptorSets(
                    *frame.descriptor_pool, *bindings.layout, bindings.private_set, &record.set, 1, 0
                );
                if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            }
            else
            {
                auto grown = growRasterDescriptorPool(slot);
                if (!grown) return cxx::unexpected(grown.error());
            }
        }
        for (std::size_t i = 0; i < bindings.uses.size(); ++i)
        {
            const auto& use = bindings.uses[i];
            if (bindings.bindings[use.field].count == 0) continue;
            const bool unsupported = use.kind != EDescriptorKind::UNIFORM_BUFFER &&
                use.kind != EDescriptorKind::STORAGE_BUFFER;
            if (unsupported || use.resource >= inputs.size()) return cxx::unexpected(RenderError{kDeviceArgument});
            const auto& source = inputs[use.resource];
            if (!source.buffer || source.size == 0) return cxx::unexpected(RenderError{kDeviceArgument});
            const auto& previous = record.inputs[i];
            const bool unchanged = previous.buffer == source.buffer && previous.offset == source.offset &&
                previous.size == source.size;
            if (unchanged) continue;
            nri::Descriptor* descriptor = nullptr;
            const nri::BufferViewDesc view{
                source.buffer,
                use.kind == EDescriptorKind::UNIFORM_BUFFER ? nri::BufferView::CONSTANT_BUFFER
                                                            : nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER,
                source.offset, source.size
            };
            const auto status = core.CreateBufferView(view, descriptor);
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            core.DestroyDescriptor(std::exchange(record.views[i], descriptor));
            record.inputs[i] = source;
            const nri::UpdateDescriptorRangeDesc write{record.set, use.binding, use.element, &descriptor, 1};
            core.UpdateDescriptorRanges(&write, 1);
            invalidateRecordings(slot);
        }
        return record.set;
    }

    RenderResult<void> RenderGraphInstance::growRasterDescriptorPool(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        const auto& core = session_->nriCore();
        nri::DescriptorPoolDesc required{};
        for (const auto& pass : frame.passes)
        {
            auto counted = FrameResources::addDescriptors(
                required, plan_->descriptor_plan_->passes[pass.compiled_pass->source_pass]
            );
            if (!counted) return cxx::unexpected(counted.error());
        }
        for (const auto& item : frame.raster_bindings)
        {
            if (auto counted = FrameResources::addDescriptors(required, *item.bindings); !counted)
                return cxx::unexpected(counted.error());
        }
        auto capacity = frame.descriptor_capacity;
        for (auto field : descriptor_counts)
        {
            if (required.*field > capacity.*field)
            {
                const auto grown = std::max<std::uint64_t>(required.*field, std::uint64_t(capacity.*field) * 2);
                capacity.*field = static_cast<std::uint32_t>(std::min<std::uint64_t>(grown, UINT32_MAX));
            }
        }
        nri::DescriptorPool* pool = nullptr;
        auto status = core.CreateDescriptorPool(session_->device(), capacity, pool);
        if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
        cxx::scope_exit rollback([&]() noexcept { core.DestroyDescriptorPool(pool); });
        std::vector<std::pair<nri::DescriptorSet**, nri::DescriptorSet*>> replacements;
        const auto allocate = [&](const PassBindings& program, nri::DescriptorSet*& destination,
                                  const auto& view) noexcept -> RenderResult<void>
        {
            if (program.private_bindings.empty()) return {};
            nri::DescriptorSet* set = nullptr;
            const auto allocated = core.AllocateDescriptorSets(
                *pool, *program.layout, program.private_set, &set, 1, 0
            );
            if (allocated != nri::Result::SUCCESS) return cxx::unexpected(nativeError(allocated));
            for (std::size_t i = 0; i < program.uses.size(); ++i)
            {
                const auto& use = program.uses[i];
                if (program.bindings[use.field].count == 0) continue;
                auto* descriptor = view(i, use);
                if (!descriptor) continue;
                const nri::UpdateDescriptorRangeDesc write{set, use.binding, use.element, &descriptor, 1};
                core.UpdateDescriptorRanges(&write, 1);
            }
            replacements.emplace_back(&destination, set);
            return {};
        };
        for (auto& pass : frame.passes)
        {
            const auto& program = plan_->descriptor_plan_->passes[pass.compiled_pass->source_pass];
            auto ready = allocate(program, pass.private_descriptors,
                [&](std::size_t i, const auto& use) noexcept
            {
                if (use.sampler != kGraphInvalidIndex) return samplers_[use.sampler].descriptor;
                // Feature preparation precedes Instance::rebind. Dirty resources already
                // name the new allocation; the old cached view is not a valid replay source.
                if (frame.bindings[use.resource].dirty) return static_cast<nri::Descriptor*>(nullptr);
                return pass.history_views[i] ? pass.history_views[i] : pass.private_views[i];
            });
            if (!ready) return cxx::unexpected(ready.error());
        }
        for (auto& item : frame.raster_bindings)
        {
            auto ready = allocate(*item.bindings, item.set,
                [&](std::size_t i, const auto&) noexcept { return item.views[i]; });
            if (!ready) return cxx::unexpected(ready.error());
        }
        // Earlier Features may already have produced CPU packets for this same writable FIF.
        // Rewire those borrows before the old pool can retire; they have not been recorded yet.
        std::unordered_map<nri::DescriptorSet*, nri::DescriptorSet*> relocated;
        for (const auto& [destination, value] : replacements)
        {
            if (*destination) relocated.emplace(*destination, value);
            *destination = value;
        }
        const auto relocate = [&](RasterDrawList& list) noexcept
        {
            for (auto*& set : list.descriptors)
            {
                if (const auto found = relocated.find(set); found != relocated.end()) set = found->second;
            }
        };
        for (auto& list : frame.raster_draws) relocate(list);
        for (auto& list : frame.draw_lists) relocate(list.draws);
        invalidateRecordings(slot);
        auto* old_pool = std::exchange(frame.descriptor_pool, pool);
        frame.descriptor_capacity = capacity;
        frame.descriptor_reserved = required;
        rollback.release();
        if (old_pool)
        {
            session_->deferRelease(
                [core = &core, old_pool]() noexcept { core->DestroyDescriptorPool(old_pool); }
            );
        }
        return {};
    }

    void RenderGraphInstance::releaseRasterBindings(std::span<const PassBindings* const> bindings) noexcept
    {
        if (bindings.empty()) return;
        const std::unordered_set<const PassBindings*> removed(bindings.begin(), bindings.end());
        std::vector<nri::Descriptor*> views;
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            auto& frame = frames_[slot];
            invalidateRecordings(slot);
            std::erase_if(frame.raster_bindings, [&](auto& record) noexcept
            {
                if (!removed.contains(record.bindings)) return false;
                views.insert(views.end(), record.views.begin(), record.views.end());
                return true;
            });
            frame.raster_binding_indices.clear();
            for (std::uint32_t index = 0; index < frame.raster_bindings.size(); ++index)
            {
                const auto& record = frame.raster_bindings[index];
                frame.raster_binding_indices[record.bindings].emplace(record.binding_key, index);
            }
        }
        if (!views.empty()) session_->deferRelease([core = &session_->nriCore(), views = std::move(views)]() noexcept
        {
            for (auto* view : views) core->DestroyDescriptor(view);
        });
    }

    void RenderGraphInstance::FrameResources::invalidateImportedBindings() noexcept
    {
        // Unique cache revisions prevent a reused instance address from matching an older binding cache.
        static std::atomic<std::uint64_t> next_revision{1};
        imported_revision = next_revision.fetch_add(1, std::memory_order_relaxed);
    }

    void RenderGraphInstance::invalidateRecording(std::uint32_t frame_slot, std::uint32_t compiled_pass) noexcept
    {
        auto& frame = frames_[frame_slot];
        if (frame.recording_revisions.empty()) return; // Inputs can precede initialize().
        const auto batch = plan_->passes()[compiled_pass].submission;
        if (compiled_pass < plan_->submissions()[batch].output_begin) ++frame.recording_revisions[batch];
    }

    void RenderGraphInstance::invalidateRecordings(std::uint32_t frame_slot) noexcept
    {
        for (auto& revision : frames_[frame_slot].recording_revisions) ++revision;
    }

    void RenderGraphInstance::invalidateRasterDraws(GraphPassId pass, std::uint32_t frame_slot) noexcept
    {
        invalidateRecording(frame_slot, plan_->pass_indices_[pass.index]);
    }

    RenderResult<void> RenderGraphInstance::createRecordings(std::uint32_t frame_slot) noexcept
    {
        auto& frame = frames_[frame_slot];
        const auto batches = plan_->submissions();
        frame.command_allocators.resize(batches.size());
        frame.command_buffers.resize(batches.size());
        frame.recording_revisions.assign(batches.size(), 1);
        frame.recorded_revisions.resize(batches.size());
        const auto& core = session_->nriCore();
        for (std::size_t batch = 0; batch < batches.size(); ++batch)
        {
            const auto& description = batches[batch];
            const bool has_tail = description.output_begin != kGraphInvalidIndex;
            const std::uint32_t first_part = has_tail && description.output_begin == description.first_pass ? 1u : 0u;
            const std::uint32_t end_part = has_tail ? 2u : 1u;
            for (auto part = first_part; part < end_part; ++part)
            {
                auto result = core.CreateCommandAllocator(
                    session_->queue(description.queue).native(), frame.command_allocators[batch][part]
                );
                if (result == nri::Result::SUCCESS)
                {
                    result = core.CreateCommandBuffer(
                        *frame.command_allocators[batch][part], frame.command_buffers[batch][part]
                    );
                }
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
        }
        return {};
    }

    void RenderGraphInstance::FrameResources::release(const nri::CoreInterface& nri_core) noexcept
    {
        for (const auto& batch : command_buffers)
            for (auto* command : batch) if (command) nri_core.DestroyCommandBuffer(command);
        for (const auto& batch : command_allocators)
            for (auto* allocator : batch) if (allocator) nri_core.DestroyCommandAllocator(allocator);
        nri_core.DestroyDescriptorPool(descriptor_pool);
        for (const auto& binding : raster_bindings)
            for (auto* view : binding.views) nri_core.DestroyDescriptor(view);
        for (auto& pass : passes)
        {
            for (auto* view : pass.private_views)
            {
                nri_core.DestroyDescriptor(view);
            }
        }
        for (auto* view : views)
        {
            nri_core.DestroyDescriptor(view);
        }
        for (auto* image : images)
        {
            nri_core.DestroyTexture(image);
        }
        for (auto* buffer : buffers)
        {
            nri_core.DestroyBuffer(buffer);
        }
        for (auto& list : draw_lists)
        {
            nri_core.DestroyBuffer(list.vertices);
            nri_core.DestroyBuffer(list.indices);
        }
        for (const auto& allocation : memories)
        {
            nri_core.FreeMemory(allocation.memory);
        }
    }
} // namespace lux::render::detail
