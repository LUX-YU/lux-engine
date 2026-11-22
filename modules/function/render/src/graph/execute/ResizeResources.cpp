#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/graph/execute/PassExtent.hpp>
#include <lux/engine/render/graph/execute/ResizeResources.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>

#include <algorithm>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <utility>

namespace lux::render::detail
{
    ResizeResources::ResizeResources(JobGpuResources& owner, ExecutionBindingId identity) noexcept
        : native_frames_(owner.frames.size()), accepted_(owner.session.makeFrontiers()), owner_(identity), session_(&owner.session)
    {
    }

    ResizeResources::ResizeResources(ResizeResources&& other) noexcept
        : output(other.output), frames(std::move(other.frames)), histories(std::move(other.histories)),
          native_frames_(std::move(other.native_frames_)), native_histories_(std::move(other.native_histories_)),
          accepted_(std::move(other.accepted_)), owner_(other.owner_), phase_(std::exchange(other.phase_, EPhase::ADOPTED)),
          session_(std::exchange(other.session_, nullptr))
    {
    }

    ResizeResources::~ResizeResources() noexcept
    {
        if (!session_)
        {
            return;
        }
        auto release = [frames = std::move(native_frames_), histories = std::move(native_histories_),
                        core = &session_->nriCore()]() mutable noexcept
        {
            for (auto& frame : frames) frame.release(*core);
            for (auto& history : histories) history.release(*core);
        };
        session_->deferRelease(std::move(accepted_), std::move(release));
    }

    RenderResult<ResizeResources> ResizeResources::prepare(
        runtime_detail::JobExecution& execution, TargetOutput output,
        std::span<const ImageReplacement> inputs
    ) noexcept
    {
        auto& gpu = execution.gpu_;
        const auto& plan = execution.graph_;
        const bool has_output = !output.frames.empty();
        const bool needs_output = plan.output() != kGraphInvalidIndex ||
            std::ranges::any_of(plan.resources(), [](const auto& resource) noexcept
            {
                return resource.target_sized;
            });
        if (needs_output && !has_output) return cxx::unexpected(RenderError{kDeviceArgument});
        const bool is_incompatible_output =
            has_output && (output.frames.size() != gpu.frames.size() || !plan.compatible(output.description));
        if (is_incompatible_output)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            const auto& input = inputs[i];
            if (input.resource.index >= plan.resources().size() || input.frames.size() != gpu.frames.size())
                return cxx::unexpected(RenderError{kFrameContract});
            const auto& resource = plan.resources()[input.resource.index];
            const bool valid_image = resource.origin == EGraphOrigin::IMPORTED &&
                std::holds_alternative<GraphImageDesc>(resource.description);
            const auto preceding = inputs.first(i);
            const auto duplicate = std::ranges::find(preceding, input.resource, &ImageReplacement::resource);
            if (!valid_image || duplicate != preceding.end())
                return cxx::unexpected(RenderError{kFrameContract});
            if (std::ranges::any_of(input.frames, [](const auto& frame) { return !frame.image; }))
                return cxx::unexpected(RenderError{kDeviceArgument});
        }
        ResizeResources result{gpu, execution.binding_identity};
        result.output = output;
        result.frames.reserve(gpu.frames.size());
        for (auto& physical : result.native_frames_) result.frames.emplace_back(physical);
        const auto& target = output.description;

        if (auto prepared = result.prepareHistories(execution); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }

        for (std::uint32_t f = 0; f < gpu.frames.size(); ++f)
        {
            auto& candidate = result.frames[f];
            const auto& live = execution.frames_[f];
            auto changes = candidate.physical.preparePhysical(
                gpu.session, {plan, target, LivePhysicalPlacement{live.bindings, live.memory_bindings}}
            );
            if (!changes) return cxx::unexpected(changes.error());
            candidate.changes = std::move(*changes);
            candidate.adopted_memories.resize(candidate.changes.memory_sources.size());
            if (plan.output() != kGraphInvalidIndex && !output.frames.empty())
            {
                FrameResourceBinding binding;
                binding.image = output.frames[f].image;
                candidate.changes.resources.push_back({plan.output(), binding});
            }
            for (const auto& input : inputs)
            {
                const auto& resource = plan.resources()[input.resource.index];
                if (auto valid = qualifyImportedImage(gpu.session, resource, *input.frames[f].image, target); !valid)
                    return cxx::unexpected(valid.error());
                FrameResourceBinding binding;
                binding.image = input.frames[f].image;
                candidate.changes.resources.push_back({input.resource.index, binding});
            }
            for (const auto& [h, history] : result.histories)
            {
                FrameResourceBinding previous;
                previous.image = history.slots[history.previous].image;
                FrameResourceBinding current;
                current.image = history.slots[history.current].image;
                candidate.changes.resources.push_back({history.plan->description.previous.index, previous});
                candidate.changes.resources.push_back({history.plan->description.current.index, current});
            }
            if (auto prepared = result.prepareBindings(execution, f); !prepared)
            {
                return cxx::unexpected(prepared.error());
            }
        }
        return result;
    }

    RenderResult<void> ResizeResources::prepareHistories(const runtime_detail::JobExecution& job) noexcept
    {
        const auto& core = session_->nriCore();
        const auto& resources = job.graph().resources();
        // Only histories whose actual physical description changed acquire new banks.
        for (std::uint32_t h = 0; h < job.histories_.size(); ++h)
        {
            const auto& previous = job.histories_[h];
            const auto resource = previous.plan->description.current.index;
            if (!resources[resource].target_sized)
            {
                continue;
            }
            auto description = graphTextureDescription(*session_, resources[resource], output.description);
            if (!description)
            {
                return cxx::unexpected(description.error());
            }
            const auto& current = core.GetTextureDesc(*previous.slots.front().image);
            if (current.width == description->width && current.height == description->height)
            {
                continue;
            }
            auto& history = histories.emplace_back(h, HistoryState{}).second;
            auto& backing = native_histories_.emplace_back();
            backing.slots.resize(previous.slots.size());
            auto created = backing.initialize(*session_, *previous.plan, resources[resource], output.description);
            if (!created) return cxx::unexpected(created.error());
            history.bindBank(*previous.plan, backing);
        }

        return {};
    }

    RenderResult<void> ResizeResources::prepareBindings(const runtime_detail::JobExecution& job, std::uint32_t f) noexcept
    {
        auto& session = *session_;
        const auto& graph = job.graph();
        const auto& core = session.nriCore();
        const auto& resources = graph.resources();
        const auto& physical = graph.physicalResources();
        const bool aliases = physical.allocation == ETransientAllocation::ALIAS_MEMORY;
        const auto& frame = job.frames_[f];
        auto& candidate = frames[f];
        std::ranges::sort(candidate.changes.resources, {}, &GraphBindingChange::index);
        const auto binding = [&](std::uint32_t index) -> const FrameResourceBinding&
        {
            const auto found =
                std::ranges::lower_bound(candidate.changes.resources, index, {}, &GraphBindingChange::index);
            const bool replaced = found != candidate.changes.resources.end() && found->index == index;
            return replaced ? found->binding : frame.bindings[index];
        };
        std::vector<std::uint8_t> affected_passes(graph.passes().size());
        for (const auto& replacement : candidate.changes.resources)
        {
            const auto index = replacement.index;
            const auto& resource = resources[index];
            for (auto pass : resource.extent_uses)
            {
                affected_passes[pass] = true;
            }
            if (resource.history != kGraphInvalidIndex)
            {
                continue; // All history views were built in the replacement bank above.
            }
            for (const auto& use : resource.image_views)
            {
                nri::Descriptor* descriptor = nullptr;
                const bool owned = index != graph.output();
                if (!owned)
                {
                    descriptor = output.frames[f].view;
                }
                else
                {
                    const bool depth = use.range.aspects != 1;
                    nri::TextureViewDesc description{
                        replacement.binding.image,
                        depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT,
                        imageFormat(std::get<GraphImageDesc>(resource.description).format).native,
                        static_cast<nri::Dim_t>(use.range.first_mip),
                        1,
                        static_cast<nri::Dim_t>(use.range.first_layer),
                        static_cast<nri::Dim_t>(use.range.layer_count)
                    };
                    const auto status = core.CreateTextureView(description, descriptor);
                    if (status != nri::Result::SUCCESS)
                    {
                        return cxx::unexpected(nativeError(status));
                    }
                    candidate.physical.views.push_back(descriptor);
                }
                candidate.views.push_back({use, descriptor, owned});
            }
            for (const auto [pass, use_index] : resource.descriptor_uses)
            {
                const auto& program = graph.bindingPlan().pass(pass);
                const auto& use = program.uses[use_index];
                nri::Descriptor* descriptor = nullptr;
                nri::Result status;
                if (replacement.binding.image)
                {
                    const auto& range = std::get<GraphImageRange>(use.range);
                    const bool arrayed = program.bindings[use.field].image_arrayed;
                    const bool storage = use.kind == EDescriptorKind::STORAGE_IMAGE;
                    nri::TextureViewDesc description{
                        replacement.binding.image,
                        storage
                            ? (arrayed ? nri::TextureView::STORAGE_TEXTURE_ARRAY : nri::TextureView::STORAGE_TEXTURE)
                            : (arrayed ? nri::TextureView::TEXTURE_ARRAY : nri::TextureView::TEXTURE),
                        imageFormat(std::get<GraphImageDesc>(resource.description).format).native,
                        static_cast<nri::Dim_t>(range.first_mip),
                        static_cast<nri::Dim_t>(range.mip_count),
                        static_cast<nri::Dim_t>(range.first_layer),
                        static_cast<nri::Dim_t>(range.layer_count)
                    };
                    description.planes = range.aspects == 1   ? nri::PlaneBits::COLOR
                                         : range.aspects == 2 ? nri::PlaneBits::DEPTH
                                                              : nri::PlaneBits::STENCIL;
                    status = core.CreateTextureView(description, descriptor);
                }
                else
                {
                    const auto& range = std::get<GraphBufferRange>(use.range);
                    const auto& source = replacement.binding;
                    const auto size =
                        use.whole_resource && use.fallback == kGraphInvalidIndex ? source.buffer_size : range.size;
                    const auto description = program.bindings[use.field].bufferView(
                        source.buffer, source.buffer_offset + range.offset, size
                    );
                    status = createGraphBufferView(*session_, description, descriptor);
                }
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
                candidate.physical.views.push_back(descriptor);
                candidate.views.push_back({{pass, use_index, EImageViewTarget::DESCRIPTOR, {}}, descriptor, true});
            }
        }
        for (std::uint32_t p = 0; p < affected_passes.size(); ++p)
        {
            if (!affected_passes[p])
            {
                continue;
            }
            candidate.extent_passes.push_back(p);
            auto prepared = validatePassExtent(session, graph, graph.passes()[p], binding);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
        }
        if (aliases)
        {
            candidate.alias_barriers = instantiateAliasBarriers(
                physical, candidate.changes.placements, graph.passes().size()
            );
            candidate.physical.memories.reserve(candidate.physical.memories.size() + frame.native.memories.size());
        }
        return {};
    }

    nri::Descriptor*&
    ResizeResources::destination(FramePass& pass, const ImageViewBinding& use) noexcept
    {
        switch (use.target)
        {
        case EImageViewTarget::DESCRIPTOR:
            return pass.private_views[use.index];
        case EImageViewTarget::COLOR:
            return pass.colors[use.index].descriptor;
        case EImageViewTarget::DEPTH:
            return pass.depth.descriptor;
        case EImageViewTarget::COLOR_RESOLVE:
            return pass.colors[use.index].resolveDst;
        case EImageViewTarget::DEPTH_RESOLVE:
            return pass.depth.resolveDst;
        case EImageViewTarget::CLEAR:
            return pass.clear_views[use.index];
        }
        std::terminate();
    }

    RenderResult<void> ResizeResources::prepareAdoption(runtime_detail::JobExecution& job) noexcept
    {
        const bool wrong_owner = owner_ != job.binding_identity || session_ != &job.gpu_.session;
        if (wrong_owner || phase_ == EPhase::ADOPTED || frames.size() != job.frames_.size())
            return cxx::unexpected(RenderError{kFrameContract});
        phase_ = EPhase::CANDIDATE;
        const auto& physical = job.graph().physicalResources();
        for (std::uint32_t f = 0; f < job.frames_.size(); ++f)
        {
            auto& frame = job.frames_[f];
            auto& candidate = frames[f];
            candidate.image_adoptions.clear();
            candidate.buffer_adoptions.clear();
            const auto binding = [&](std::uint32_t resource) -> const FrameResourceBinding&
            {
                const auto changed =
                    std::ranges::lower_bound(candidate.changes.resources, resource, {}, &GraphBindingChange::index);
                return changed != candidate.changes.resources.end() && changed->index == resource ? changed->binding
                                                                                          : frame.bindings[resource];
            };
            const auto find_slot = [](const auto& objects, const auto* object) noexcept
            {
                const auto found = std::ranges::find(objects, object);
                return found == objects.end() ? SIZE_MAX : static_cast<std::size_t>(found - objects.begin());
            };
            for (std::uint32_t s = 0; s < candidate.changes.replaced_slots.size(); ++s)
            {
                if (!candidate.changes.replaced_slots[s]) continue;
                const auto index = physical.slots[s].resource.index;
                const auto& old = frame.bindings[index];
                const auto& incoming = binding(index);
                const bool is_image = old.image != nullptr;
                auto& adoptions = is_image ? candidate.image_adoptions : candidate.buffer_adoptions;
                const auto swap = is_image
                    ? ResizeResources::OwnershipSlots{
                        find_slot(frame.native.images, old.image), find_slot(candidate.physical.images, incoming.image)
                    }
                    : ResizeResources::OwnershipSlots{
                        find_slot(frame.native.buffers, old.buffer),
                        find_slot(candidate.physical.buffers, incoming.buffer)
                    };
                if (swap.live == SIZE_MAX || swap.candidate == SIZE_MAX)
                    return cxx::unexpected(RenderError{kFrameContract, {f, index}});
                adoptions.push_back(swap);
            }
            for (auto& view : candidate.views)
            {
                if (!view.owned) continue;
                auto* previous = ResizeResources::destination(frame.passes[view.use.pass], view.use);
                const bool is_attachment = view.use.target != EImageViewTarget::DESCRIPTOR;
                view.ownership = {
                    is_attachment ? find_slot(frame.native.views, previous) : SIZE_MAX,
                    find_slot(candidate.physical.views, view.descriptor)
                };
                if (view.ownership.candidate == SIZE_MAX || (is_attachment && view.ownership.live == SIZE_MAX))
                    return cxx::unexpected(RenderError{kFrameContract, {f, view.use.pass, view.use.index}});
            }
            for (const auto pass : candidate.extent_passes)
            {
                auto ready = validatePassExtent(*session_, job.graph(), job.graph().passes()[pass], binding);
                if (!ready)
                {
                    return ready;
                }
            }
        }
        phase_ = EPhase::READY;
        return {};
    }

    void ResizeResources::commit(runtime_detail::JobExecution& job) && noexcept
    {
        const bool wrong_owner = owner_ != job.binding_identity || session_ != &job.gpu_.session;
        if (wrong_owner || phase_ != EPhase::READY) std::terminate();
        phase_ = EPhase::ADOPTED;
        accepted_.merge(job.gpu_.accepted);
        const auto& core = session_->nriCore();
        for (std::uint32_t f = 0; f < job.frames_.size(); ++f)
        {
            auto& frame = job.frames_[f];
            auto& candidate = frames[f];
            for (const auto [live, incoming] : candidate.image_adoptions)
                std::swap(frame.native.images[live], candidate.physical.images[incoming]);
            for (const auto [live, incoming] : candidate.buffer_adoptions)
                std::swap(frame.native.buffers[live], candidate.physical.buffers[incoming]);
            if (!candidate.changes.placements.empty())
            {
                for (std::uint32_t group = 0; group < candidate.adopted_memories.size(); ++group)
                {
                    const auto source = candidate.changes.memory_sources[group];
                    auto& incoming = source.owner == EAllocationOwner::LIVE ? frame.native.memories[source.index]
                                                                           : candidate.physical.memories[source.index];
                    candidate.adopted_memories[group] = std::exchange(incoming, {});
                }
                // All new allocations have moved. Only abandoned old groups remain to be released.
                candidate.physical.memories.clear();
                for (auto& memory : frame.native.memories)
                {
                    if (memory.memory)
                    {
                        candidate.physical.memories.push_back(std::exchange(memory, {}));
                    }
                }
                frame.native.memories = std::move(candidate.adopted_memories);
                frame.memory_bindings = std::move(candidate.changes.placements);
                for (std::uint32_t p = 0; p < frame.passes.size(); ++p)
                {
                    auto& current = frame.passes[p].alias_barrier;
                    const auto& incoming = candidate.alias_barriers[p];
                    const bool same_scope = !current || !incoming ||
                        (current->before.access == incoming->before.access &&
                         current->before.stages == incoming->before.stages &&
                         current->after.access == incoming->after.access &&
                         current->after.stages == incoming->after.stages);
                    if (current.has_value() != incoming.has_value() || !same_scope) frame.invalidatePass(job.graph(), p);
                    current = incoming;
                }
            }
            for (const auto& replacement : candidate.changes.resources)
            {
                frame.invalidateResource(job.graph().recordingDependencies(), replacement.index);
                auto& binding = frame.bindings[replacement.index];
                frame.native.collectRasterViews(
                    binding.buffer != replacement.binding.buffer ? binding.buffer : nullptr,
                    binding.image != replacement.binding.image ? binding.image : nullptr,
                    candidate.physical.views
                );
                // Do not overwrite parameter values or unrelated Storage updates made while Resize waited.
                binding.select(replacement.binding.buffer, replacement.binding.image, core);
                binding.buffer_offset = replacement.binding.buffer_offset;
                binding.buffer_size = replacement.binding.buffer_size;
                binding.dirty = false;
                std::erase(frame.changed_resources, replacement.index);
                const auto& resource = job.graph().resources()[replacement.index];
                for (const auto [p, index] : resource.barrier_uses)
                {
                    if (binding.image)
                    {
                        frame.passes[p].images[index].texture = binding.image;
                    }
                    else
                    {
                        frame.passes[p].buffers[index].buffer = binding.buffer;
                    }
                }
                for (auto index : resource.final_barrier_uses)
                {
                    if (binding.image)
                    {
                        frame.final_images[index].texture = binding.image;
                    }
                    else
                    {
                        frame.final_buffers[index].buffer = binding.buffer;
                    }
                }
            }
            for (auto& view : candidate.views)
            {
                auto& pass = frame.passes[view.use.pass];
                auto& destination = ResizeResources::destination(pass, view.use);
                auto* previous = std::exchange(destination, view.descriptor);
                if (view.owned)
                {
                    candidate.physical.views[view.ownership.candidate] = previous;
                    if (view.ownership.live != SIZE_MAX)
                        frame.native.views[view.ownership.live] = view.descriptor;
                }
                if (view.use.target == EImageViewTarget::DESCRIPTOR)
                {
                    const auto& use =
                        job.graph().bindingPlan().pass(pass.compiled_pass->binding_slot).uses[view.use.index];
                    const nri::UpdateDescriptorRangeDesc
                        update{pass.private_descriptors, use.binding, use.element, &destination, 1};
                    core.UpdateDescriptorRanges(&update, 1);
                    pass.history_views[view.use.index] = nullptr;
                    const auto history = job.graph().resources()[use.resource].history;
                    if (history != kGraphInvalidIndex) frame.pending_history_views.mark(history);
                }
            }
            frame.record_ready = false;
            frame.invalidateImportedBindings();
            std::ranges::fill(frame.history_valid, 0);
        }
        for (std::size_t h = 0; h < histories.size(); ++h)
        {
            auto& [index, replacement] = histories[h];
            for (std::size_t f = 0; f < job.frames_.size(); ++f)
            {
                for (const auto& old : job.histories_[index].slots)
                {
                    job.frames_[f].native.collectRasterViews(old.buffer, old.image, frames[f].physical.views);
                }
                job.frames_[f].pending_history_clears.mark(index);
                job.frames_[f].pending_history_views.mark(index);
            }
            std::swap(job.gpu_.histories[index], native_histories_[h]);
            std::swap(job.histories_[index], replacement);
        }
        for (auto& history : job.histories_)
        {
            history.invalidate = true;
        }
        job.output_ = output;
        for (std::uint32_t slot = 0; slot < job.frames_.size(); ++slot)
        {
            job.frames_[slot].bindHistory(job.histories_, job.graph().recordingDependencies());
            job.frames_[slot].updateHistoryViews(job.histories_, session_->nriCore());
        }
    }

} // namespace lux::render::detail
