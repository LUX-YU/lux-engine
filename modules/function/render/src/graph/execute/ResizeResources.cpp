#include <lux/engine/render/graph/execute/PassExtent.hpp>
#include <lux/engine/render/graph/execute/ResizeResources.hpp>

#include <algorithm>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <utility>

namespace lux::render::detail
{
    RenderGraphInstance::ResizeResources::ResizeResources(JobGpuResources& owner) noexcept
        : native_frames_(owner.frames.size()), accepted_(owner.session.makeFrontiers()), owner_(&owner), session_(&owner.session)
    {
    }

    RenderGraphInstance::ResizeResources::ResizeResources(ResizeResources&& other) noexcept
        : output(other.output), frames(std::move(other.frames)), histories(std::move(other.histories)),
          native_frames_(std::move(other.native_frames_)), native_histories_(std::move(other.native_histories_)),
          accepted_(std::move(other.accepted_)), owner_(other.owner_), prepared_(std::exchange(other.prepared_, false)),
          session_(std::exchange(other.session_, nullptr))
    {
    }

    RenderGraphInstance::ResizeResources::~ResizeResources() noexcept
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

    RenderResult<RenderGraphInstance::ResizeResources> RenderGraphInstance::ResizeResources::prepare(
        JobGpuResources& gpu, RenderGraphInstance& execution, TargetOutput output,
        std::span<const ImageReplacement> inputs
    ) noexcept
    {
        if (&execution.gpu_ != &gpu) return cxx::unexpected(RenderError{kFrameContract});
        const auto& plan = *execution.plan_;
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
        ResizeResources result{gpu};
        result.output = output;
        result.frames.reserve(gpu.frames.size());
        for (auto& physical : result.native_frames_) result.frames.emplace_back(physical);
        const auto& target = output.description;

        if (auto prepared = execution.prepareResizeHistories(result, target); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }

        for (std::uint32_t f = 0; f < gpu.frames.size(); ++f)
        {
            auto& candidate = result.frames[f];
            if (auto prepared = execution.prepareResizePhysical(result, f, target); !prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            if (plan.output() != kGraphInvalidIndex && !output.frames.empty())
            {
                Binding binding;
                binding.image = output.frames[f].image;
                candidate.resources.push_back({plan.output(), binding});
            }
            for (const auto& input : inputs)
            {
                if (auto valid = execution.qualifyImportedImage(input.resource, *input.frames[f].image, target); !valid)
                    return cxx::unexpected(valid.error());
                Binding binding;
                binding.image = input.frames[f].image;
                candidate.resources.push_back({input.resource.index, binding});
            }
            for (const auto& [h, history] : result.histories)
            {
                Binding previous;
                previous.image = history.slots[history.previous].image;
                Binding current;
                current.image = history.slots[history.current].image;
                candidate.resources.push_back({history.plan->description.previous.index, previous});
                candidate.resources.push_back({history.plan->description.current.index, current});
            }
            if (auto prepared = execution.prepareResizeBindings(result, f); !prepared)
            {
                return cxx::unexpected(prepared.error());
            }
        }
        return result;
    }

    RenderResult<void> RenderGraphInstance::prepareResizeHistories(
        ResizeResources& result,
        const TargetDesc& target
    ) noexcept
    {
        const auto& core = session_->nriCore();
        const auto& resources = plan_->resources();
        // Only histories whose actual physical description changed acquire new banks.
        for (std::uint32_t h = 0; h < histories_.size(); ++h)
        {
            const auto& previous = histories_[h];
            const auto resource = previous.plan->description.current.index;
            if (!resources[resource].target_sized)
            {
                continue;
            }
            auto description = textureDescription(resources[resource], target);
            if (!description)
            {
                return cxx::unexpected(description.error());
            }
            const auto& current = core.GetTextureDesc(*previous.slots.front().image);
            if (current.width == description->width && current.height == description->height)
            {
                continue;
            }
            auto& history = result.histories.emplace_back(h, HistoryState{}).second;
            history.plan = previous.plan;
            auto& backing = result.native_histories_.emplace_back();
            backing.slots.resize(previous.slots.size());
            history.slots = backing.slots;
            history.sample_sequences.resize(history.slots.size());
            for (auto& slot : history.slots)
            {
                slot.last_use = session_->makeFrontiers();
                const auto status =
                    core.CreatePlacedTexture(session_->device(), NriDeviceHeap, *description, slot.image);
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
            }
            if (auto created = createHistoryViews(history); !created)
            {
                return cxx::unexpected(created.error());
            }
        }

        return {};
    }

    RenderResult<void> RenderGraphInstance::prepareResizePhysical(
        ResizeResources& result,
        std::uint32_t f,
        const TargetDesc& target
    ) noexcept
    {
        auto& session = *session_;
        const auto& core = session.nriCore();
        const auto& resources = plan_->resources();
        const auto& physical = plan_->physical_resources_;
        const bool aliases = physical.allocation == ETransientAllocation::ALIAS_MEMORY;
        const auto& frame = frames_[f];
        auto& candidate = result.frames[f];
        std::vector<Binding> replacements(physical.slots.size());
        candidate.replaced_slots.resize(physical.slots.size());
        std::vector<nri::MemoryDesc> requirements(aliases ? physical.slots.size() : 0);
        const auto create = [&](std::uint32_t s) -> RenderResult<void>
        {
            const auto index = physical.slots[s].resource.index;
            const auto& resource = resources[index];
            auto& binding = replacements[s];
            if (!aliases)
            {
                auto made = createResource(index, binding, target);
                if (!made)
                {
                    return made;
                }
            }
            else
            {
                nri::Result status;
                if (std::holds_alternative<GraphImageDesc>(resource.description))
                {
                    auto description = textureDescription(resource, target);
                    if (!description)
                    {
                        return cxx::unexpected(description.error());
                    }
                    status = core.CreateTexture(session.device(), *description, binding.image);
                }
                else
                {
                    binding.buffer_size = std::get<GraphBufferDesc>(resource.description).size;
                    const auto description = bufferDescription(resource);
                    if (!description)
                    {
                        return cxx::unexpected(description.error());
                    }
                    status = core.CreateBuffer(session.device(), *description, binding.buffer);
                }
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
            }
            if (binding.image)
            {
                candidate.physical.images.push_back(binding.image);
            }
            else
            {
                candidate.physical.buffers.push_back(binding.buffer);
            }
            return {};
        };
        for (std::uint32_t s = 0; s < physical.slots.size(); ++s)
        {
            const auto index = physical.slots[s].resource.index;
            const auto& resource = resources[index];
            const auto& binding = frame.bindings[index];
            if (resource.target_sized)
            {
                auto description = textureDescription(resource, target);
                if (!description)
                {
                    return cxx::unexpected(description.error());
                }
                const auto& current = core.GetTextureDesc(*binding.image);
                candidate.replaced_slots[s] =
                    current.width != description->width || current.height != description->height;
                if (candidate.replaced_slots[s])
                {
                    if (auto made = create(s); !made)
                    {
                        return cxx::unexpected(made.error());
                    }
                }
            }
            if (aliases)
            {
                const auto& selected = candidate.replaced_slots[s] ? replacements[s] : binding;
                if (selected.image)
                {
                    core.GetTextureMemoryDesc(*selected.image, nri::MemoryLocation::DEVICE, requirements[s]);
                }
                else
                {
                    core.GetBufferMemoryDesc(*selected.buffer, nri::MemoryLocation::DEVICE, requirements[s]);
                }
            }
        }
        if (aliases)
        {
            const auto placements = instantiatePlacements(
                physical, requirements, session_->description().graphicsAPI
            );
            expandMemoryMigration(frame.memory_bindings, placements, candidate.replaced_slots);
            for (std::uint32_t slot = 0; slot < physical.slots.size(); ++slot)
            {
                if (candidate.replaced_slots[slot] && !replacements[slot].image && !replacements[slot].buffer)
                {
                    if (auto made = create(slot); !made)
                    {
                        return cxx::unexpected(made.error());
                    }
                }
            }
            std::vector<nri::AllocateMemoryDesc> allocations(physical.slots.size());
            for (std::uint32_t s = 0; s < physical.slots.size(); ++s)
            {
                auto& allocation = allocations[placements[s].root.index];
                allocation.size = std::max(allocation.size, requirements[s].size);
                allocation.type = requirements[s].type;
                allocation.vma.enable = !requirements[s].mustBeDedicated;
                allocation.vma.alignment = std::max(allocation.vma.alignment, requirements[s].alignment);
                const auto* image =
                    std::get_if<GraphImageDesc>(&resources[physical.slots[s].resource.index].description);
                allocation.allowMultisampleTextures |= image && image->samples > 1;
            }
            std::vector<MemoryGroupId> root_to_group(placements.size());
            for (std::uint32_t s = 0; s < physical.slots.size(); ++s)
            {
                if (placements[s].root.index != s)
                {
                    continue;
                }
                root_to_group[s] = {static_cast<std::uint32_t>(candidate.adopted_memories.size())};
                candidate.adopted_memories.push_back({});
                candidate.previous_allocations.push_back(frame.memory_bindings[s].group);
                candidate.allocation_candidates.push_back(kGraphInvalidIndex);
                if (candidate.replaced_slots[s])
                {
                    nri::Memory* memory = nullptr;
                    const auto status = core.AllocateMemory(session.device(), allocations[s], memory);
                    if (status != nri::Result::SUCCESS)
                    {
                        return cxx::unexpected(nativeError(status));
                    }
                    candidate.allocation_candidates.back() =
                        static_cast<std::uint32_t>(candidate.physical.memories.size());
                    candidate.physical.memories.push_back({memory, allocations[s].size});
                }
            }
            std::vector<nri::BindTextureMemoryDesc> images;
            std::vector<nri::BindBufferMemoryDesc> buffers;
            candidate.placements.resize(placements.size());
            for (std::uint32_t s = 0; s < physical.slots.size(); ++s)
            {
                const auto& placement = placements[s];
                const auto group = root_to_group[placement.root.index];
                candidate.placements[s] = {group, placement.reason};
                if (!candidate.replaced_slots[s])
                {
                    continue;
                }
                auto* memory = candidate.physical.memories[candidate.allocation_candidates[group.index]].memory;
                const auto& binding = replacements[s];
                if (binding.image)
                {
                    images.push_back({binding.image, memory, 0});
                }
                else
                {
                    buffers.push_back({binding.buffer, memory, 0});
                }
            }
            if (!images.empty())
            {
                const auto status = core.BindTextureMemory(images.data(), static_cast<std::uint32_t>(images.size()));
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
            }
            if (!buffers.empty())
            {
                const auto status = core.BindBufferMemory(buffers.data(), static_cast<std::uint32_t>(buffers.size()));
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
            }
        }
        for (std::uint32_t r = 0; r < resources.size(); ++r)
        {
            const auto slot = physical.resource_slots[r];
            if (slot.valid() && candidate.replaced_slots[slot.index])
            {
                candidate.resources.push_back({r, replacements[slot.index]});
            }
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::prepareResizeBindings(ResizeResources& result, std::uint32_t f) noexcept
    {
        auto& session = *session_;
        const auto& core = session.nriCore();
        const auto& resources = plan_->resources();
        const auto& physical = plan_->physical_resources_;
        const bool aliases = physical.allocation == ETransientAllocation::ALIAS_MEMORY;
        const auto& frame = frames_[f];
        auto& candidate = result.frames[f];
        const auto output = result.output;
        std::ranges::sort(candidate.resources, {}, &ResizeResources::Resource::index);
        const auto binding = [&](std::uint32_t index) -> const Binding&
        {
            const auto found =
                std::ranges::lower_bound(candidate.resources, index, {}, &ResizeResources::Resource::index);
            return found != candidate.resources.end() && found->index == index ? found->binding : frame.bindings[index];
        };
        std::vector<std::uint8_t> affected_passes(plan_->passes().size());
        for (const auto& replacement : candidate.resources)
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
                const bool owned = index != plan_->output();
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
                const auto& program = plan_->binding_plan_->pass(pass);
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
                    status = createBufferView(description, descriptor);
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
            auto prepared = validatePassExtent(session, *plan_, plan_->passes()[p], binding);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
        }
        if (aliases)
        {
            candidate.alias_barriers = instantiateAliasBarriers(physical, candidate.placements, plan_->passes().size());
            candidate.physical.memories.reserve(candidate.physical.memories.size() + frame.native.memories.size());
        }
        return {};
    }

    nri::Descriptor*& RenderGraphInstance::ResizeResources::destination(Pass& pass, const ImageViewBinding& use) noexcept
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

    RenderResult<void> RenderGraphInstance::prepareResizeAdoption(ResizeResources& result) noexcept
    {
        if (result.owner_ != &gpu_ || result.frames.size() != frames_.size())
            return cxx::unexpected(RenderError{kFrameContract});
        result.prepared_ = false;
        const auto& physical = plan_->physical_resources_;
        for (std::uint32_t f = 0; f < frames_.size(); ++f)
        {
            auto& frame = frames_[f];
            auto& candidate = result.frames[f];
            candidate.image_adoptions.clear();
            candidate.buffer_adoptions.clear();
            const auto binding = [&](std::uint32_t resource) -> const Binding&
            {
                const auto changed =
                    std::ranges::lower_bound(candidate.resources, resource, {}, &ResizeResources::Resource::index);
                return changed != candidate.resources.end() && changed->index == resource ? changed->binding
                                                                                          : frame.bindings[resource];
            };
            const auto find_slot = [](const auto& objects, const auto* object) noexcept
            {
                const auto found = std::ranges::find(objects, object);
                return found == objects.end() ? SIZE_MAX : static_cast<std::size_t>(found - objects.begin());
            };
            for (std::uint32_t s = 0; s < candidate.replaced_slots.size(); ++s)
            {
                if (!candidate.replaced_slots[s]) continue;
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
                auto ready = validatePassExtent(*session_, *plan_, plan_->passes()[pass], binding);
                if (!ready)
                {
                    return ready;
                }
            }
        }
        result.prepared_ = true;
        return {};
    }

    void RenderGraphInstance::commitResize(ResizeResources&& result) noexcept
    {
        if (result.owner_ != &gpu_ || !std::exchange(result.prepared_, false)) std::terminate();
        result.accepted_.merge(gpu_.accepted);
        const auto& core = session_->nriCore();
        for (std::uint32_t f = 0; f < frames_.size(); ++f)
        {
            auto& frame = frames_[f];
            auto& candidate = result.frames[f];
            for (const auto [live, incoming] : candidate.image_adoptions)
                std::swap(frame.native.images[live], candidate.physical.images[incoming]);
            for (const auto [live, incoming] : candidate.buffer_adoptions)
                std::swap(frame.native.buffers[live], candidate.physical.buffers[incoming]);
            if (!candidate.placements.empty())
            {
                for (std::uint32_t group = 0; group < candidate.adopted_memories.size(); ++group)
                {
                    const auto new_memory = candidate.allocation_candidates[group];
                    auto& incoming = new_memory == kGraphInvalidIndex
                                         ? frame.native.memories[candidate.previous_allocations[group].index]
                                         : candidate.physical.memories[new_memory];
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
                frame.memory_bindings = std::move(candidate.placements);
                for (std::uint32_t p = 0; p < frame.passes.size(); ++p)
                {
                    auto& current = frame.passes[p].alias_barrier;
                    const auto& incoming = candidate.alias_barriers[p];
                    const bool same_scope = !current || !incoming ||
                        (current->before.access == incoming->before.access &&
                         current->before.stages == incoming->before.stages &&
                         current->after.access == incoming->after.access &&
                         current->after.stages == incoming->after.stages);
                    if (current.has_value() != incoming.has_value() || !same_scope) invalidateRecording(f, p);
                    current = incoming;
                }
            }
            for (const auto& replacement : candidate.resources)
            {
                invalidateResourceRecordings(f, replacement.index);
                auto& binding = frame.bindings[replacement.index];
                collectRasterViews(
                    frame,
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
                const auto& resource = plan_->resources()[replacement.index];
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
                        plan_->binding_plan_->pass(pass.compiled_pass->binding_slot).uses[view.use.index];
                    const nri::UpdateDescriptorRangeDesc
                        update{pass.private_descriptors, use.binding, use.element, &destination, 1};
                    core.UpdateDescriptorRanges(&update, 1);
                    pass.history_views[view.use.index] = nullptr;
                    const auto history = plan_->resources()[use.resource].history;
                    if (history != kGraphInvalidIndex) frame.pending_history_views.mark(history);
                }
            }
            frame.record_ready = false;
            frame.invalidateImportedBindings();
            std::ranges::fill(frame.history_valid, 0);
        }
        for (std::size_t h = 0; h < result.histories.size(); ++h)
        {
            auto& [index, replacement] = result.histories[h];
            for (std::size_t f = 0; f < frames_.size(); ++f)
            {
                for (const auto& old : histories_[index].slots)
                {
                    collectRasterViews(frames_[f], old.buffer, old.image, result.frames[f].physical.views);
                }
                frames_[f].pending_history_clears.mark(index);
                frames_[f].pending_history_views.mark(index);
            }
            std::swap(gpu_.histories[index], result.native_histories_[h]);
            std::swap(histories_[index], replacement);
        }
        for (auto& history : histories_)
        {
            history.invalidate = true;
        }
        output_ = result.output;
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            bindHistory(slot);
            updateHistoryViews(slot);
        }
    }

    void RenderGraphInstance::releaseOutputReferences() noexcept
    {
        const auto resource = plan_->output();
        const auto& uses = plan_->resources()[resource];
        std::vector<nri::Descriptor*> views;
        for (auto& frame : frames_)
        {
            collectRasterViews(frame, nullptr, frame.bindings[resource].image, views);
            frame.bindings[resource].select(nullptr, nullptr, session_->nriCore());
            for (auto* output : frame.output_views)
            {
                *output = nullptr;
            }
            for (const auto [pass, index] : uses.barrier_uses)
            {
                frame.passes[pass].images[index].texture = nullptr;
            }
            for (auto index : uses.final_barrier_uses)
            {
                frame.final_images[index].texture = nullptr;
            }
            for (const auto [pass, index] : uses.descriptor_uses)
            {
                auto*& descriptor = frame.passes[pass].private_views[index];
                if (descriptor)
                {
                    views.push_back(std::exchange(descriptor, nullptr));
                }
            }
            invalidateResourceRecordings(static_cast<std::uint32_t>(&frame - frames_.data()), resource);
            frame.invalidateImportedBindings();
        }
        output_.frames = {};
        session_->deferRelease(
            gpu_.accepted,
            [views = std::move(views), core = &session_->nriCore()]() noexcept
            {
                for (auto* view : views)
                {
                    core->DestroyDescriptor(view);
                }
            }
        );
    }
} // namespace lux::render::detail
