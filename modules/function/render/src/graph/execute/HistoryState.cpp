#include <algorithm>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>

namespace lux::render::detail
{
    void RenderGraphInstance::HistoryState::release(const nri::CoreInterface& core) noexcept
    {
        for (auto& slot : slots)
        {
            for (auto* view : slot.views)
            {
                core.DestroyDescriptor(view);
            }
            for (auto* view : slot.initial_views)
            {
                core.DestroyDescriptor(view);
            }
            core.DestroyTexture(slot.image);
            core.DestroyBuffer(slot.buffer);
        }
    }

    bool RenderGraphInstance::HistoryState::select(const DeviceSession& session) noexcept
    {
        const auto reusable = [&](const HistoryState::HistorySlot& slot)
        {
            return session.reached(slot.last_use);
        };

        if (invalidate)
        {
            latest = kGraphInvalidIndex;
            invalidate = false;
        }
        previous = latest;
        current = kGraphInvalidIndex;
        for (std::uint32_t i = 0; i < slots.size(); ++i)
        {
            if (i == previous || !reusable(slots[i]))
            {
                continue;
            }
            if (previous == kGraphInvalidIndex)
            {
                previous = i;
            }
            else
            {
                current = i;
                break;
            }
        }
        if (current == kGraphInvalidIndex)
        {
            return false;
        }
        clear_previous = latest == kGraphInvalidIndex;
        clear_current = !slots[current].initialized;
        return true;
    }

    void RenderGraphInstance::HistoryState::publish() noexcept
    {
        auto& written = slots[current];
        written.sample_sequence = ++sequence;
        latest = current;
    }

    RenderResult<void> RenderGraphInstance::createHistories() noexcept
    {
        histories_.resize(plan_->histories_.size());
        for (std::size_t index = 0; index < histories_.size(); ++index)
        {
            auto& history = histories_[index];
            history.plan = &plan_->histories_[index];
            history.slots.resize(frames_.size() + 1);
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
                    const auto& program = plan_->descriptor_plan_->passes[pass];
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
                    const auto& plan = plan_->descriptor_plan_->passes[pass];
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
        for (std::uint32_t p = 0; p < plan_->passes().size(); ++p)
        {
            const auto& pass = plan_->passes()[p];
            const auto& uses = plan_->descriptor_plan_->passes[pass.binding_slot].uses;
            for (std::uint32_t i = 0; i < uses.size(); ++i)
            {
                const auto& use = uses[i];
                if (auto created = addView({use.resource}, use.range, p, i, Target::DESCRIPTOR, use.kind); !created)
                {
                    return created;
                }
            }
            if (const auto* raster = pass.rasterScope())
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
            if (const auto* transfer = plan_->transferCommands(pass))
            {
                for (std::uint32_t i = 0; i < transfer->operations.size(); ++i)
                {
                    if (const auto* clear = std::get_if<ClearImage>(&transfer->operations[i]))
                    {
                        const auto& access = clear->destination;
                        if (auto created = addView(
                                access.version.resource,
                                access.range,
                                p,
                                i,
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
        }
        // Initialization views are immutable and shared by the safe frame descriptor banks.
        for (auto& slot : history.slots)
        {
            if (!slot.image)
            {
                continue;
            }
            const auto& image = core.GetTextureDesc(*slot.image);
            const auto& graph_image =
                std::get<GraphImageDesc>(plan_->resources()[description.current.index].description);
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
        for (std::size_t h = 0; h < histories_.size(); ++h)
        {
            const auto& history = histories_[h];
            frame.history_valid[h] = history.latest != kGraphInvalidIndex;
            const auto& description = history.plan->description;
            for (const auto [resource, selected] :
                 {std::pair{description.previous, history.previous}, std::pair{description.current, history.current}})
            {
                const bool was_dirty = frame.bindings[resource.index].dirty;
                const auto& physical = history.slots[selected];
                frame.bindings[resource.index].image = physical.image;
                frame.bindings[resource.index].buffer = physical.buffer;
                frame.bindings[resource.index].buffer_size = physical.buffer_size;
                frame.bindings[resource.index].dirty = true;
                if (!was_dirty)
                {
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
        for (std::size_t h = 0; h < histories_.size(); ++h)
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
                        plan_->descriptor_plan_->passes[pass.compiled_pass->binding_slot].uses[use.index];
                    if (!frame.history_valid[h] && binding.fallback != kGraphInvalidIndex)
                    {
                        view = pass.private_views[use.index];
                    }
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
    }

    RenderResult<void> RenderGraphInstance::invalidateHistory(std::span<const GraphResourceId> previous) noexcept
    {
        // Validate the whole request before changing any instance state.
        for (auto resource : previous)
        {
            const bool valid = resource.index < plan_->resources().size() &&
                               plan_->resources()[resource.index].origin == EGraphOrigin::HISTORY_PREVIOUS &&
                               plan_->resources()[resource.index].history != kGraphInvalidIndex;
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
                histories_[plan_->resources()[resource.index].history].invalidate = true;
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
        for (auto& history : histories_)
        {
            const bool produced = std::ranges::all_of(history.plan->writers, [&](auto writer)
            {
                const auto region = plan_->passes()[writer].condition_region;
                return region == kGraphInvalidIndex || frame.enabled_regions[region] != 0;
            });
            if (produced)
            {
                history.publish();
            }
        }
    }

    void RenderGraphInstance::acceptedBatch(std::uint32_t batch, SubmissionPoint point) noexcept
    {
        uses_.accept(point);
        const auto mark = [&](HistoryState::HistorySlot& slot)
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
        for (auto resource : plan_->submissions_[batch].histories)
        {
            const auto& compiled = plan_->resources_[resource.index];
            auto& history = histories_[compiled.history];
            mark(history.slots[compiled.origin == EGraphOrigin::HISTORY_PREVIOUS ? history.previous : history.current]);
        }
    }
} // namespace lux::render::detail
