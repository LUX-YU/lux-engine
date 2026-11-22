#include <algorithm>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>

namespace lux::render::detail
{
    bool RenderGraphInstance::HistoryState::select(const DeviceSession& session) noexcept
    {
        if (invalidate)
        {
            retained_samples = 0;
            latest = kGraphInvalidIndex;
            invalidate = false;
        }
        previous = kGraphInvalidIndex;
        current = kGraphInvalidIndex;
        const auto distance = plan->description.distance;
        const auto oldest = retained_samples ? sequence - (retained_samples - 1) : 0;
        clear_previous = retained_samples < distance;
        for (std::uint32_t i = 0; i < slots.size(); ++i)
        {
            const auto& slot = slots[i];
            if (oldest != 0 && sample_sequences[i] >= oldest)
            {
                if (!clear_previous && sample_sequences[i] == oldest)
                {
                    previous = i;
                }
                continue;
            }
            if (!session.reached(slot.last_use))
            {
                continue;
            }
            if (clear_previous && previous == kGraphInvalidIndex)
            {
                previous = i;
            }
            else if (current == kGraphInvalidIndex)
            {
                current = i;
            }
        }
        if (previous == kGraphInvalidIndex || current == kGraphInvalidIndex)
        {
            return false;
        }
        clear_current = !slots[current].initialized;
        return true;
    }

    void RenderGraphInstance::HistoryState::publish() noexcept
    {
        if (sequence == UINT64_MAX)
        {
            std::terminate();
        }
        sample_sequences[current] = ++sequence;
        latest = current;
        if (retained_samples < plan->description.distance)
        {
            ++retained_samples;
        }
    }

    RenderResult<void> RenderGraphInstance::createHistories() noexcept
    {
        histories_.resize(graph_.histories_.size());
        gpu_.histories.resize(histories_.size());
        for (std::size_t index = 0; index < histories_.size(); ++index)
        {
            auto& history = histories_[index];
            history.plan = &graph_.histories_[index];
            const auto distance = history.plan->description.distance;
            if (frames_.size() > kGraphInvalidIndex - distance)
            {
                return cxx::unexpected(RenderError{kDeviceCapacity});
            }
            auto& backing = gpu_.histories[index];
            backing.slots.resize(frames_.size() + distance);
            history.slots = backing.slots;
            history.sample_sequences.resize(history.slots.size());
            for (auto& slot : history.slots)
            {
                slot.last_use = session_->makeFrontiers();
                Binding candidate;
                if (auto created = createResource(history.plan->description.current.index, candidate); !created)
                {
                    return created;
                }
                slot.image = candidate.image;
                slot.buffer = candidate.buffer;
                slot.buffer_size = candidate.buffer_size;
                if (slot.buffer) slot.buffer_description = &session_->nriCore().GetBufferDesc(*slot.buffer);
            }
            if (auto views = createHistoryViews(history); !views)
            {
                return views;
            }
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::createHistoryViews(HistoryState& history) noexcept
    {
        using Target = EImageViewTarget;
        const auto& core = session_->nriCore();
        const auto& description = history.plan->description;
        const auto belongs = [&](GraphResourceId id)
        { return id == description.previous || id == description.current; };
        const auto addView = [&](GraphResourceId resource,
                                 const VGraphRange& range,
                                 std::uint32_t pass,
                                 std::uint32_t index,
                                 Target target,
                                 EDescriptorKind kind) -> RenderResult<void>
        {
            if (!belongs(resource))
            {
                return {};
            }
            history.views.push_back({resource, pass, index, target});
            for (auto& slot : history.slots)
            {
                nri::Descriptor* view = nullptr;
                nri::Result result;
                if (slot.image)
                {
                    const auto& selected = std::get<GraphImageRange>(range);
                    const bool attachment = target != Target::DESCRIPTOR;
                    const bool depth = selected.aspects != 1;
                    const auto type =
                        attachment
                            ? (depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT)
                            : (kind == EDescriptorKind::STORAGE_IMAGE ? nri::TextureView::STORAGE_TEXTURE
                                                                      : nri::TextureView::TEXTURE);
                    const auto& program = graph_.binding_plan_->pass(pass);
                    const bool arrayed =
                        target == Target::DESCRIPTOR && program.bindings[program.uses[index].field].image_arrayed;
                    const auto view_type =
                        arrayed ? (kind == EDescriptorKind::STORAGE_IMAGE ? nri::TextureView::STORAGE_TEXTURE_ARRAY
                                                                          : nri::TextureView::TEXTURE_ARRAY)
                                : type;
                    nri::TextureViewDesc input{
                        slot.image,
                        view_type,
                        core.GetTextureDesc(*slot.image).format,
                        static_cast<nri::Dim_t>(selected.first_mip),
                        static_cast<nri::Dim_t>(selected.mip_count),
                        static_cast<nri::Dim_t>(selected.first_layer),
                        static_cast<nri::Dim_t>(selected.layer_count)
                    };
                    input.planes = selected.aspects == 1   ? nri::PlaneBits::COLOR
                                   : selected.aspects == 2 ? nri::PlaneBits::DEPTH
                                                           : nri::PlaneBits::STENCIL;
                    result = core.CreateTextureView(input, view);
                }
                else
                {
                    const auto& selected = std::get<GraphBufferRange>(range);
                    const auto& plan = graph_.binding_plan_->pass(pass);
                    const auto& shader = plan.bindings[plan.uses[index].field];
                    const auto input = shader.bufferView(slot.buffer, selected.offset, selected.size);
                    result = createBufferView(input, view);
                }
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
                slot.views.push_back(view);
            }
            return {};
        };
        for (std::uint32_t p = 0; p < graph_.passes().size(); ++p)
        {
            const auto& pass = graph_.passes()[p];
            const auto& uses = graph_.binding_plan_->pass(pass.binding_slot).uses;
            for (std::uint32_t i = 0; i < uses.size(); ++i)
            {
                const auto& use = uses[i];
                if (auto created = addView({use.resource}, use.range, p, i, Target::DESCRIPTOR, use.kind); !created)
                {
                    return created;
                }
            }
            if (const auto* raster = graph_.recordingProgram().rasterScope(pass))
            {
                const auto addAttachment = [&](const RasterAttachment& attachment, std::uint32_t i, bool depth
                                           ) -> RenderResult<void>
                {
                    const auto& access = attachment.access;
                    if (auto view = addView(
                            access.version.resource,
                            access.range,
                            p,
                            i,
                            depth ? Target::DEPTH : Target::COLOR,
                            EDescriptorKind::SAMPLED_IMAGE
                        );
                        !view)
                    {
                        return view;
                    }
                    if (attachment.resolve)
                    {
                        const auto& resolve = *attachment.resolve;
                        return addView(
                            resolve.version.resource,
                            resolve.range,
                            p,
                            i,
                            depth ? Target::DEPTH_RESOLVE : Target::COLOR_RESOLVE,
                            EDescriptorKind::SAMPLED_IMAGE
                        );
                    }
                    return {};
                };
                for (std::uint32_t i = 0; i < raster->colors.size(); ++i)
                {
                    if (auto created = addAttachment(raster->colors[i], i, false); !created)
                    {
                        return created;
                    }
                }
                if (raster->depth)
                {
                    if (auto created = addAttachment(*raster->depth, 0, true); !created)
                    {
                        return created;
                    }
                }
            }
            if (const auto* transfer = graph_.transferCommands(pass))
            {
                const auto& transfers = graph_.recordingProgram().transfers;
                for (const auto& clear : transfers.clearCommands(*transfer))
                {
                    const auto& access = clear.destination;
                    if (auto created = addView(
                            GraphResourceId{access.slot},
                            access.range,
                            p,
                            clear.view,
                            Target::CLEAR,
                            EDescriptorKind::SAMPLED_IMAGE
                        );
                        !created)
                    {
                        return created;
                    }
                }
            }
        }
        // Initialization views are immutable and shared by the safe frame descriptor banks.
        for (auto& slot : history.slots)
        {
            if (!slot.image)
            {
                continue;
            }
            const auto& image = core.GetTextureDesc(*slot.image);
            slot.image_description = &image;
            const auto& graph_image =
                std::get<GraphImageDesc>(graph_.resources()[description.current.index].description);
            const bool depth = rdesc::textureAspectMask(graph_image.format) != 1;
            for (nri::Dim_t mip = 0; mip < image.mipNum; ++mip)
            {
                nri::Descriptor* view = nullptr;
                const nri::TextureViewDesc input{
                    slot.image,
                    depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT,
                    image.format,
                    mip,
                    1,
                    0,
                    image.layerNum
                };
                const auto result = core.CreateTextureView(input, view);
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
                slot.initial_views.push_back(view);
            }
        }
        return {};
    }

    bool RenderGraphInstance::selectHistory() noexcept
    {
        return std::ranges::all_of(histories_, [&](auto& history) { return history.select(*session_); });
    }

    void RenderGraphInstance::bindHistory(std::uint32_t frame_slot) noexcept
    {
        auto& frame = frames_[frame_slot];
        for (std::uint32_t h = 0; h < histories_.size(); ++h)
        {
            const auto& history = histories_[h];
            const auto clear = [&](bool required, std::uint32_t slot) noexcept
            {
                return required ? FrameResources::HistoryClear{slot, history.slots[slot].initialized}
                                : FrameResources::HistoryClear{};
            };
            const std::array clears{clear(history.clear_previous, history.previous),
                                    clear(history.clear_current, history.current)};
            if (frame.history_clears[h] != clears)
            {
                frame.history_clears[h] = clears;
                frame.pending_history_clears.mark(h);
            }
            const bool valid = !history.clear_previous;
            const bool validity_changed = static_cast<bool>(frame.history_valid[h]) != valid;
            frame.history_valid[h] = valid;
            const auto& description = history.plan->description;
            for (const auto [resource, selected] :
                 {std::pair{description.previous, history.previous}, std::pair{description.current, history.current}})
            {
                const auto& physical = history.slots[selected];
                auto& binding = frame.bindings[resource.index];
                const bool unchanged = binding.image == physical.image && binding.buffer == physical.buffer &&
                    binding.buffer_size == physical.buffer_size;
                if (unchanged && !validity_changed) continue;
                frame.pending_history_views.mark(h);
                binding.image = physical.image;
                binding.image_description = physical.image_description;
                binding.buffer = physical.buffer;
                binding.buffer_description = physical.buffer_description;
                binding.buffer_size = physical.buffer_size;
                invalidateResourceRecordings(frame_slot, resource.index);
                if (!binding.dirty)
                {
                    binding.dirty = true;
                    frame.changed_resources.push_back(resource.index);
                }
            }
        }
    }

    void RenderGraphInstance::updateHistoryViews(std::uint32_t frame_slot) noexcept
    {
        using Target = EImageViewTarget;
        auto& frame = frames_[frame_slot];
        const auto& core = session_->nriCore();
        for (const auto h : frame.pending_history_views.slots())
        {
            const auto& history = histories_[h];
            const auto& description = history.plan->description;
            for (std::size_t i = 0; i < history.views.size(); ++i)
            {
                const auto& use = history.views[i];
                const auto selected = use.resource == description.previous ? history.previous : history.current;
                auto* view = history.slots[selected].views[i];
                auto& pass = frame.passes[use.pass];
                switch (use.target)
                {
                case Target::DESCRIPTOR:
                {
                    const auto& binding =
                        graph_.binding_plan_->pass(pass.compiled_pass->binding_slot).uses[use.index];
                    if (!frame.history_valid[h] && binding.fallback != kGraphInvalidIndex)
                    {
                        view = pass.private_views[use.index];
                    }
                    if (pass.history_views[use.index] == view) break;
                    const nri::UpdateDescriptorRangeDesc
                        update{pass.private_descriptors, binding.binding, binding.element, &view, 1};
                    core.UpdateDescriptorRanges(&update, 1);
                    pass.history_views[use.index] = view;
                    break;
                }
                case Target::COLOR:
                    pass.colors[use.index].descriptor = view;
                    break;
                case Target::DEPTH:
                    pass.depth.descriptor = view;
                    break;
                case Target::COLOR_RESOLVE:
                    pass.colors[use.index].resolveDst = view;
                    break;
                case Target::DEPTH_RESOLVE:
                    pass.depth.resolveDst = view;
                    break;
                case Target::CLEAR:
                    pass.clear_views[use.index] = view;
                    break;
                }
            }
        }
        frame.pending_history_views.clear();
    }

    RenderResult<void> RenderGraphInstance::invalidateHistory(std::span<const GraphResourceId> previous) noexcept
    {
        // Validate the whole request before changing any instance state.
        for (auto resource : previous)
        {
            const bool valid = resource.index < graph_.resources().size() &&
                               graph_.resources()[resource.index].origin == EGraphOrigin::HISTORY_PREVIOUS &&
                               graph_.resources()[resource.index].history != kGraphInvalidIndex;
            if (!valid)
            {
                return cxx::unexpected(RenderError{kGraphUnknownResource});
            }
        }
        if (previous.empty())
        {
            for (auto& history : histories_)
            {
                history.invalidate = true;
            }
        }
        else
        {
            for (auto resource : previous)
            {
                histories_[graph_.resources()[resource.index].history].invalidate = true;
            }
        }
        return {};
    }

    void RenderGraphInstance::publishHistory(std::uint32_t frame_slot) noexcept
    {
        if (histories_.empty())
        {
            return;
        }
        const auto& frame = frames_[frame_slot];
        for (std::size_t i = 0; i < histories_.size(); ++i)
        {
            if (frame.publish_history[i])
            {
                histories_[i].publish();
            }
        }
    }

    void RenderGraphInstance::acceptedBatch(std::uint32_t slot, std::uint32_t batch, SubmissionPoint point) noexcept
    {
        gpu_.accepted.accept(point);
        for (auto id : graph_.submissions_[batch].imported_resources)
            if (auto* uses = frames_[slot].selectedUses(id.index).accepted) uses->accept(point);
        const auto mark = [&](JobHistoryResources::Slot& slot)
        {
            slot.last_use.accept(point);
            slot.initialized = true;
        };
        if (batch == 0)
        {
            for (auto& history : histories_)
            {
                if (history.clear_previous)
                {
                    mark(history.slots[history.previous]);
                }
                if (history.clear_current)
                {
                    mark(history.slots[history.current]);
                }
            }
        }
        for (auto resource : graph_.submissions_[batch].histories)
        {
            const auto& compiled = graph_.resources_[resource.index];
            auto& history = histories_[compiled.history];
            mark(history.slots[compiled.origin == EGraphOrigin::HISTORY_PREVIOUS ? history.previous : history.current]);
        }
    }
} // namespace lux::render::detail
