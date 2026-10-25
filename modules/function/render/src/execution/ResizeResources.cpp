#include <lux/engine/render/execution/PassExtent.hpp>
#include <lux/engine/render/execution/ResizeResources.hpp>

#include <algorithm>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/PipelineLayoutPlan.hpp>
#include <utility>

namespace lux::render::detail
{
    RenderGraphInstance::ResizeResources::ResizeResources(DeviceSession& session) noexcept : session_(&session) {}

    RenderGraphInstance::ResizeResources::ResizeResources(ResizeResources&& other) noexcept
        : output(other.output), frames(std::move(other.frames)), histories(std::move(other.histories)),
          session_(std::exchange(other.session_, nullptr))
    {
    }

    RenderGraphInstance::ResizeResources::~ResizeResources() noexcept
    {
        if (!session_)
        {
            return;
        }
        session_->deferRelease(
            [frames = std::move(frames), histories = std::move(histories), core = &session_->nriCore()](
            ) mutable noexcept
            {
                for (auto& frame : frames)
                {
                    frame.physical.release(*core);
                }
                for (auto& [index, history] : histories)
                {
                    history.release(*core);
                }
            }
        );
    }

    RenderResult<RenderGraphInstance::ResizeResources> RenderGraphInstance::prepareResize(
        TargetOutput output,
        std::span<const ImageReplacement> inputs
    ) noexcept
    {
        const bool has_output = !output.frames.empty();
        const bool is_incompatible_output =
            has_output && (output.frames.size() != frames_.size() || !plan_->compatible(output.description));
        if (is_incompatible_output)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        auto& session = *session_;
        ResizeResources result{session};
        result.output = output;
        result.frames.resize(frames_.size());
        const auto target = output.frames.empty() ? target_ : output.description;

        if (auto prepared = prepareResizeHistories(result, target); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }

        for (std::uint32_t f = 0; f < frames_.size(); ++f)
        {
            auto& candidate = result.frames[f];
            if (auto prepared = prepareResizePhysical(result, f, target); !prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            if (plan_->output() != kGraphInvalidIndex && !output.frames.empty())
            {
                Binding binding;
                binding.image = output.frames[f].image;
                candidate.resources.push_back({plan_->output(), binding});
            }
            for (const auto& input : inputs)
            {
                const auto& expected = std::get<GraphImageDesc>(plan_->resources()[input.resource.index].description);
                const auto& actual = session.nriCore().GetTextureDesc(*input.frames[f].image);
                const bool extent_mismatch = expected.extent.source != EImageExtentSource::IMPORTED &&
                                             (actual.width != expected.width || actual.height != expected.height);
                const bool description_mismatch =
                    extent_mismatch || actual.format != imageFormat(expected.format).native ||
                    actual.mipNum != expected.mips || actual.layerNum != expected.layers ||
                    actual.sampleNum != expected.samples;
                if (description_mismatch)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
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
            if (auto prepared = prepareResizeBindings(result, f); !prepared)
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
            history.slots.resize(previous.slots.size());
            for (auto& slot : history.slots)
            {
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
                    const nri::BufferDesc description{binding.buffer_size, 4, resource.buffer_usage};
                    status = core.CreateBuffer(session.device(), description, binding.buffer);
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
            candidate.placements = instantiatePlacements(physical, resources, plan_->recording_, requirements);
            expandMemoryMigration(frame.memory_bindings, candidate.placements, candidate.replaced_slots);
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
                auto& allocation = allocations[candidate.placements[s].allocation];
                allocation.size = std::max(allocation.size, requirements[s].size);
                allocation.type = requirements[s].type;
                allocation.vma.enable = !requirements[s].mustBeDedicated;
                allocation.vma.alignment = std::max(allocation.vma.alignment, requirements[s].alignment);
                const auto* image =
                    std::get_if<GraphImageDesc>(&resources[physical.slots[s].resource.index].description);
                allocation.allowMultisampleTextures |= image && image->samples > 1;
            }
            std::vector<std::uint32_t> compact(physical.slots.size());
            for (std::uint32_t s = 0; s < physical.slots.size(); ++s)
            {
                if (candidate.placements[s].allocation != s)
                {
                    continue;
                }
                compact[s] = static_cast<std::uint32_t>(candidate.adopted_memories.size());
                candidate.adopted_memories.push_back({});
                candidate.previous_allocations.push_back(frame.memory_bindings[s].allocation);
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
            for (std::uint32_t s = 0; s < physical.slots.size(); ++s)
            {
                candidate.placements[s].allocation = compact[candidate.placements[s].allocation];
                if (!candidate.replaced_slots[s])
                {
                    continue;
                }
                const auto group = candidate.placements[s].allocation;
                auto* memory = candidate.physical.memories[candidate.allocation_candidates[group]].memory;
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
            if (slot != kGraphInvalidIndex && candidate.replaced_slots[slot])
            {
                candidate.resources.push_back({r, replacements[slot]});
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
                const auto& program = plan_->descriptor_plan_->passes[plan_->passes()[pass].source_pass];
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
                    const nri::BufferViewDesc description{
                        source.buffer,
                        use.kind == EDescriptorKind::UNIFORM_BUFFER ? nri::BufferView::CONSTANT_BUFFER
                        : use.kind == EDescriptorKind::TYPED_STORAGE_BUFFER
                            ? nri::BufferView::STORAGE_BUFFER
                            : nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER,
                        source.buffer_offset + range.offset,
                        size,
                        use.kind == EDescriptorKind::TYPED_STORAGE_BUFFER ? nri::Format::R32_UINT : nri::Format::UNKNOWN
                    };
                    status = core.CreateBufferView(description, descriptor);
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
            auto& extent = candidate.extents.emplace_back();
            extent.pass = p;
            auto prepared = preparePassExtent(
                session,
                *plan_,
                plan_->passes()[p],
                binding,
                extent.dispatch_groups,
                extent.copy_layouts
            );
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
        }
        if (aliases)
        {
            candidate.alias_barriers.resize(plan_->passes().size());
            std::vector<std::uint32_t> tails(candidate.adopted_memories.size(), kGraphInvalidIndex);
            for (std::uint32_t slot = 0; slot < candidate.placements.size(); ++slot)
            {
                auto& previous = tails[candidate.placements[slot].allocation];
                if (previous != kGraphInvalidIndex)
                {
                    for (std::size_t q = 0; q < physical.slots[slot].first_pass.size(); ++q)
                    {
                        const auto pass = physical.slots[slot].first_pass[q];
                        if (pass == kGraphInvalidIndex)
                        {
                            continue;
                        }
                        auto& barrier = candidate.alias_barriers[pass];
                        if (!barrier)
                        {
                            barrier.emplace();
                        }
                        barrier->before.access |= physical.slots[previous].accesses[q];
                        barrier->after.access |= physical.slots[slot].accesses[q];
                        barrier->before.stages = barrier->after.stages = nri::StageBits::ALL;
                    }
                }
                previous = slot;
            }
            candidate.physical.memories.reserve(candidate.physical.memories.size() + frame.memories.size());
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::prepareResizeAdoption(ResizeResources& result) noexcept
    {
        for (std::uint32_t f = 0; f < frames_.size(); ++f)
        {
            const auto& frame = frames_[f];
            auto& candidate = result.frames[f];
            const auto binding = [&](std::uint32_t resource) -> const Binding&
            {
                const auto changed =
                    std::ranges::lower_bound(candidate.resources, resource, {}, &ResizeResources::Resource::index);
                return changed != candidate.resources.end() && changed->index == resource ? changed->binding
                                                                                          : frame.bindings[resource];
            };
            for (auto& extent : candidate.extents)
            {
                auto ready = preparePassExtent(
                    *session_,
                    *plan_,
                    plan_->passes()[extent.pass],
                    binding,
                    extent.dispatch_groups,
                    extent.copy_layouts
                );
                if (!ready)
                {
                    return ready;
                }
            }
        }
        return {};
    }

    void RenderGraphInstance::commitResize(ResizeResources&& result) noexcept
    {
        const auto& core = session_->nriCore();
        const auto& physical = plan_->physical_resources_;
        for (std::uint32_t f = 0; f < frames_.size(); ++f)
        {
            auto& frame = frames_[f];
            auto& candidate = result.frames[f];
            for (std::uint32_t s = 0; s < candidate.replaced_slots.size(); ++s)
            {
                if (!candidate.replaced_slots[s])
                {
                    continue;
                }
                const auto index = physical.slots[s].resource.index;
                const auto replacement =
                    std::ranges::lower_bound(candidate.resources, index, {}, &ResizeResources::Resource::index);
                const auto& old = frame.bindings[index];
                if (old.image)
                {
                    auto& current = *std::ranges::find(frame.images, old.image);
                    auto& incoming = *std::ranges::find(candidate.physical.images, replacement->binding.image);
                    std::swap(current, incoming);
                }
                else
                {
                    auto& current = *std::ranges::find(frame.buffers, old.buffer);
                    auto& incoming = *std::ranges::find(candidate.physical.buffers, replacement->binding.buffer);
                    std::swap(current, incoming);
                }
            }
            if (!candidate.placements.empty())
            {
                for (std::uint32_t group = 0; group < candidate.adopted_memories.size(); ++group)
                {
                    const auto new_memory = candidate.allocation_candidates[group];
                    auto& incoming = new_memory == kGraphInvalidIndex
                                         ? frame.memories[candidate.previous_allocations[group]]
                                         : candidate.physical.memories[new_memory];
                    candidate.adopted_memories[group] = std::exchange(incoming, {});
                }
                // All new allocations have moved. Only abandoned old groups remain to be released.
                candidate.physical.memories.clear();
                for (auto& memory : frame.memories)
                {
                    if (memory.memory)
                    {
                        candidate.physical.memories.push_back(std::exchange(memory, {}));
                    }
                }
                frame.memories = std::move(candidate.adopted_memories);
                frame.memory_bindings = std::move(candidate.placements);
                for (std::uint32_t p = 0; p < frame.passes.size(); ++p)
                {
                    frame.passes[p].alias_barrier = candidate.alias_barriers[p];
                }
            }
            for (const auto& replacement : candidate.resources)
            {
                auto& binding = frame.bindings[replacement.index];
                // Do not overwrite parameter values or unrelated Storage updates made while Resize waited.
                binding.image = replacement.binding.image;
                binding.buffer = replacement.binding.buffer;
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
                nri::Descriptor** destination = nullptr;
                switch (view.use.target)
                {
                case EImageViewTarget::DESCRIPTOR:
                    destination = &pass.private_views[view.use.index];
                    break;
                case EImageViewTarget::COLOR:
                    destination = &pass.colors[view.use.index].descriptor;
                    break;
                case EImageViewTarget::DEPTH:
                    destination = &pass.depth.descriptor;
                    break;
                case EImageViewTarget::COLOR_RESOLVE:
                    destination = &pass.colors[view.use.index].resolveDst;
                    break;
                case EImageViewTarget::DEPTH_RESOLVE:
                    destination = &pass.depth.resolveDst;
                    break;
                case EImageViewTarget::CLEAR:
                    destination = &pass.clear_views[view.use.index];
                    break;
                }
                auto* previous = std::exchange(*destination, view.descriptor);
                if (view.owned)
                {
                    auto& incoming = *std::ranges::find(candidate.physical.views, view.descriptor);
                    incoming = previous;
                    if (view.use.target != EImageViewTarget::DESCRIPTOR)
                    {
                        *std::ranges::find(frame.views, previous) = view.descriptor;
                    }
                }
                if (view.use.target == EImageViewTarget::DESCRIPTOR)
                {
                    const auto& use =
                        plan_->descriptor_plan_->passes[pass.compiled_pass->source_pass].uses[view.use.index];
                    const nri::UpdateDescriptorRangeDesc
                        update{pass.private_descriptors, use.binding, use.element, destination, 1};
                    core.UpdateDescriptorRanges(&update, 1);
                }
            }
            for (auto& extent : candidate.extents)
            {
                auto& pass = frame.passes[extent.pass];
                pass.dispatch_groups = extent.dispatch_groups;
                pass.copy_layouts = std::move(extent.copy_layouts);
            }
            frame.invalidateImportedBindings();
            std::ranges::fill(frame.history_valid, 0);
        }
        for (auto& [index, replacement] : result.histories)
        {
            std::swap(histories_[index], replacement);
        }
        for (auto& history : histories_)
        {
            history.invalidate = true;
        }
        output_ = result.output;
        if (!output_.frames.empty())
        {
            target_ = output_.description;
        }
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
            frame.bindings[resource].image = nullptr;
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
            frame.invalidateImportedBindings();
        }
        output_.frames = {};
        session_->deferRelease(
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
