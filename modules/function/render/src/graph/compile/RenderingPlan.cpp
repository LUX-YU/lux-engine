#include <lux/engine/render/graph/compile/GraphAnalysis.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/graph/compile/RenderingPlan.hpp>
#include <lux/engine/render/graph/compile/GraphCompileWorkspace.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>

namespace lux::render::detail
{
    namespace
    {
        bool continuesAttachment(const RasterAttachment& previous, const RasterAttachment& next) noexcept
        {
            return !previous.resolve && !next.resolve &&
                   previous.access.version.resource == next.access.version.resource &&
                   previous.access.range == next.access.range && previous.store == EAttachmentStore::STORE &&
                   next.load == EAttachmentLoad::LOAD;
        }

        bool attachmentTransition(const ImageTransition& transition, const RasterScope& work) noexcept
        {
            const auto& barrier = transition.native;
            if (barrier.before.layout != barrier.after.layout)
            {
                return false;
            }
            const auto matches = [&](const RasterAttachment& attachment)
            {
                const auto& range = std::get<GraphImageRange>(attachment.access.range);
                return attachment.access.version.resource.index == transition.resource &&
                       barrier.mipOffset >= range.first_mip && barrier.mipOffset < range.first_mip + range.mip_count &&
                       barrier.layerOffset >= range.first_layer &&
                       barrier.layerOffset < range.first_layer + range.layer_count;
            };
            return std::ranges::any_of(work.colors, matches) || (work.depth && matches(*work.depth));
        }
    } // namespace

    RenderResult<void> planRendering(
        const RenderGraphAnalysis& analysis,
        std::span<CompiledResource> resources,
        RecordingProgram& recording
    ) noexcept
    {
        // Ported from Legacy RenderPassPlanner::plan / collect_pass_refs:
        // consecutive live graphics work only. Identity and subranges strengthen its format-only key.
        std::vector<std::vector<const GraphVersionAccess*>> readers(resources.size());
        std::vector<std::vector<const GraphLifetime*>> lifetimes(resources.size());
        for (const auto& use : analysis.versions)
        {
            readers[use.version.resource.index].push_back(&use);
        }
        for (const auto& lifetime : analysis.lifetimes)
        {
            lifetimes[lifetime.version.resource.index].push_back(&lifetime);
        }
        for (auto& uses : readers)
        {
            std::ranges::sort(uses, {}, [](const auto* use) { return use->version.number; });
        }
        for (auto& uses : lifetimes)
        {
            std::ranges::sort(uses, std::greater{}, [](const auto* use) { return use->last; });
        }
        recording.groups.clear();
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            auto& pass = recording.passes[p];
            auto* raster = recording.rasterScope(pass);
            if (!raster)
            {
                continue;
            }
            const auto validateStore = [&](const RasterAttachment& attachment)
            {
                if (attachment.store == EAttachmentStore::STORE)
                {
                    return true;
                }
                const auto uses = std::ranges::equal_range(
                    readers[attachment.access.version.resource.index],
                    attachment.access.version.number,
                    std::less{},
                    [](const auto* use) { return use->version.number; }
                );
                return std::ranges::none_of(
                    uses,
                    [&](const auto* use)
                    {
                        return use->version == attachment.access.version &&
                               use->producer == analysis.execution_order[p] &&
                               detail::overlaps(use->range, attachment.access.range);
                    }
                );
            };
            const bool invalid_store = !std::ranges::all_of(raster->colors, validateStore) ||
                                       (raster->depth && !validateStore(*raster->depth));
            if (invalid_store)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }

            const auto* previous = p ? recording.rasterScope(recording.passes[p - 1]) : nullptr;
            bool same_attachments = previous && pass.submission == recording.passes[p - 1].submission &&
                                    pass.condition_region == recording.passes[p - 1].condition_region &&
                                    previous->colors.size() == raster->colors.size() &&
                                    bool(previous->depth) == bool(raster->depth);
            // Exclusive bodies own their sections and cannot share a managed rendering scope.
            same_attachments = same_attachments && !raster->exclusive && !previous->exclusive;
            if (same_attachments)
            {
                for (std::size_t i = 0; i < raster->colors.size(); ++i)
                {
                    same_attachments &= continuesAttachment(previous->colors[i], raster->colors[i]);
                }
                if (raster->depth)
                {
                    same_attachments &= continuesAttachment(*previous->depth, *raster->depth);
                }
            }
            const bool attachment_barriers_only =
                pass.entry_buffers.count == 0 &&
                std::ranges::all_of(
                    recording.imageBarriers(pass),
                    [&](const auto& barrier) { return attachmentTransition(barrier, *raster); }
                );
            if (same_attachments && attachment_barriers_only)
            {
                pass.rendering_boundary = ERenderingBoundary::CONTINUATION;
                pass.rendering_group = recording.passes[p - 1].rendering_group;
                auto& group = recording.groups[pass.rendering_group];
                ++group.pass_count;
                for (std::size_t i = 0; i < group.colors.size(); ++i)
                {
                    group.colors[i].store = raster->colors[i].store;
                }
                if (group.depth)
                {
                    group.depth->store = raster->depth->store;
                }
                // Attachment accesses are ordered inside one rendering scope, not by illegal in-scope barriers.
                pass.entry_images.count = 0;
            }
            else
            {
                pass.rendering_boundary = !previous           ? ERenderingBoundary::FIRST_RASTER
                                          : !same_attachments ? ERenderingBoundary::ATTACHMENT_OR_LOAD_STORE
                                                              : ERenderingBoundary::RESOURCE_DEPENDENCY;
                pass.rendering_group = static_cast<std::uint32_t>(recording.groups.size());
                recording.groups.push_back({p, 1, raster->colors, raster->depth});
            }
        }
        // Legacy computeAttachmentOps: only execution-order last-use can justify discarding transient storage.
        // Author LOAD/CLEAR is preserved; graph exports count as observations after the final pass.
        for (auto& group : recording.groups)
        {
            const auto end = group.first_pass + group.pass_count - 1;
            const auto deriveStore = [&](RasterAttachment& attachment)
            {
                const auto resource = attachment.access.version.resource;
                if (resources[resource.index].origin != EGraphOrigin::TRANSIENT)
                {
                    return;
                }
                const auto& uses = lifetimes[resource.index];
                const auto end_of_later_uses =
                    std::ranges::partition_point(uses, [&](const auto* use) { return use->last > end; });
                const bool needed = std::ranges::any_of(
                    std::ranges::subrange(uses.begin(), end_of_later_uses),
                    [&](const auto* use)
                    { return use->last > end && detail::overlaps(use->range, attachment.access.range); }
                );
                if (!needed)
                {
                    attachment.store = EAttachmentStore::DISCARD;
                }
            };
            for (auto& color : group.colors)
            {
                deriveStore(color);
            }
            if (group.depth)
            {
                deriveStore(*group.depth);
            }
        }
        for (auto& resource : resources)
        {
            resource.image_views.clear();
            resource.extent_uses.clear();
        }
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            const auto extent = [&](GraphResourceId resource)
            {
                auto& uses = resources[resource.index].extent_uses;
                if (uses.empty() || uses.back() != p)
                {
                    uses.push_back(p);
                }
            };
            const auto view = [&](const ResourceAccess& access, std::uint32_t index, EImageViewTarget target)
            {
                auto& resource = resources[access.version.resource.index];
                resource.image_views.push_back({p, index, target, std::get<GraphImageRange>(access.range)});
                extent(access.version.resource);
            };
            const auto& pass = recording.passes[p];
            if (const auto* slot = std::get_if<RasterCommandSlot>(&pass.commands))
            {
                auto* raster = &recording.raster_scopes[slot->index];
                const auto& group = recording.groups[pass.rendering_group];
                const auto* previous = p ? recording.rasterScope(recording.passes[p - 1]) : nullptr;
                raster->reset_viewport = group.first_pass == p ||
                    (previous && (previous->viewport != raster->viewport || previous->exclusive));
                for (std::uint32_t i = 0; i < group.colors.size(); ++i)
                {
                    view(group.colors[i].access, i, EImageViewTarget::COLOR);
                    if (group.colors[i].resolve)
                    {
                        view(*group.colors[i].resolve, i, EImageViewTarget::COLOR_RESOLVE);
                    }
                }
                if (group.depth)
                {
                    view(group.depth->access, 0, EImageViewTarget::DEPTH);
                    if (group.depth->resolve)
                    {
                        view(*group.depth->resolve, 0, EImageViewTarget::DEPTH_RESOLVE);
                    }
                }
            }
            else if (const auto* compute = recording.computeCommands(pass))
            {
                if (const auto* dispatch = std::get_if<ImageDispatchExtent>(&compute->dispatch))
                {
                    extent({dispatch->image});
                }
            }
            else if (const auto* transfer = recording.transferCommands(pass))
            {
                const auto& transfers = recording.transfers;
                for (const auto packet : transfers.commands(*transfer))
                {
                    if (packet.opcode == ETransferOpcode::CLEAR_COLOR || packet.opcode == ETransferOpcode::CLEAR_DEPTH)
                    {
                        const auto& clear = transfers.clears[packet.operand];
                        const auto slot = clear.destination.slot;
                        resources[slot].image_views.push_back({
                            p, clear.view, EImageViewTarget::CLEAR, clear.destination.range
                        });
                        extent({slot});
                    }
                    else if (packet.opcode == ETransferOpcode::COPY_IMAGE)
                    {
                        const auto& copy = transfers.image_copies[packet.operand];
                        extent({copy.source.slot});
                        extent({copy.destination.slot});
                    }
                    else if (packet.opcode == ETransferOpcode::UPLOAD || packet.opcode == ETransferOpcode::READBACK)
                    {
                        const auto& copy = transfers.image_buffers[packet.operand];
                        extent({copy.image.slot});
                        extent({copy.buffer.slot});
                    }
                }
            }
        }
        return {};
    }

    namespace
    {
        // Facts true on every path reaching the next command in this physical CB.
        struct FixedBindingState final
        {
            nri::PipelineLayout* layout = nullptr;
            nri::Pipeline* pipeline = nullptr;
            nri::BindPoint point = nri::BindPoint::INHERIT;
            bool pool = false, mixed = false;
            std::array<std::uint8_t, 4> sets{}; // Zero is unknown, otherwise shared kind + 1.

            void intersect(const FixedBindingState& skipped) noexcept
            {
                const bool same_bindings = pool == skipped.pool && layout == skipped.layout && point == skipped.point &&
                    pipeline == skipped.pipeline && sets == skipped.sets;
                mixed = mixed || skipped.mixed || !same_bindings;
                pool = pool && skipped.pool;
                if (pipeline != skipped.pipeline) pipeline = nullptr;
                if (layout != skipped.layout || point != skipped.point)
                {
                    layout = nullptr;
                    point = nri::BindPoint::INHERIT;
                    sets = {};
                    return;
                }
                for (std::size_t i = 0; i < sets.size(); ++i)
                    if (sets[i] != skipped.sets[i]) sets[i] = 0;
            }

            FixedBindingDelta select(
                const NativeProgramSlot& program, bool has_private, const CompiledPass& pass
            ) noexcept
            {
                FixedBindingDelta delta;
                mixed = false;
                delta.pool = !pool;
                pool = true;
                const auto& binding = program.descriptors;
                if (!binding.layout) return delta;
                delta.point = std::holds_alternative<RasterCommandSlot>(pass.commands) ?
                    nri::BindPoint::GRAPHICS : nri::BindPoint::COMPUTE;
                delta.layout = layout != binding.layout || point != delta.point;
                if (delta.layout || delta.pool) sets = {};
                layout = binding.layout;
                point = delta.point;
                delta.pipeline = program.pipeline && program.pipeline != pipeline;
                if (program.pipeline) pipeline = program.pipeline;
                for (std::uint8_t kind = 0; kind < binding.shared_sets.size(); ++kind)
                {
                    const auto set = binding.shared_sets[kind];
                    if (set == kGraphInvalidIndex) continue;
                    const auto identity = static_cast<std::uint8_t>(kind + 1);
                    if (sets[set] != identity) delta.shared_sets |= static_cast<std::uint8_t>(1u << kind);
                    sets[set] = identity;
                }
                // Private sets and inline storage are independently writable for each pass and FIF.
                delta.private_set = has_private;
                if (has_private) sets[binding.private_set] = 0;
                delta.constants = pass.parameter_size != 0;
                return delta;
            }
        };
    } // namespace

    void RenderGraphCompiler::Workspace::compileFixedBindings(compiled::RenderGraph& graph) noexcept
    {
        auto& recording = graph.recording_;
        const auto& programs = graph.programs_;
        const auto& bindings = *graph.binding_plan_;
        for (const auto& batch : graph.submissions())
        {
            for (const auto& part : batch.recordings)
            {
                FixedBindingState state, skipped;
                auto region = kGraphInvalidIndex, last = kGraphInvalidIndex;
                auto skipped_pass = kGraphInvalidIndex, skipped_join = kGraphInvalidIndex;
                for (auto p = part.first_pass; p < part.end_pass; ++p)
                {
                    auto& pass = recording.passes[p];
                    const auto* transfer = recording.transferCommands(pass);
                    // A copy cannot change the predecessor on either conditional path. Resolve
                    // their join at the next command that actually consumes or changes bindings.
                    if (transfer && transfer->preserves_bindings) continue;
                    const auto* raster = recording.rasterCommands(pass);
                    const bool dynamic = raster && std::holds_alternative<RasterListCommands>(raster->draw);
                    const auto select = [&](FixedBindingState& facts)
                    {
                        if (dynamic) return FixedBindingDelta{.pool = !facts.pool};
                        const auto slot = pass.binding_slot;
                        return facts.select(programs[slot], bindings.passes[slot].has_private_set, pass);
                    };
                    if (region != pass.condition_region)
                    {
                        if (region != kGraphInvalidIndex)
                        {
                            auto taken = state;
                            state.intersect(skipped);
                            if (state.mixed)
                            {
                                pass.binding_join = static_cast<std::uint32_t>(recording.binding_joins.size());
                                recording.conditions[region].binding_join = pass.binding_join;
                                recording.binding_joins.push_back({
                                    p, last, skipped_pass, skipped_join, select(taken), select(skipped)
                                });
                            }
                        }
                        region = pass.condition_region;
                        if (region != kGraphInvalidIndex)
                        {
                            skipped = state;
                            skipped_pass = last;
                            skipped_join = pass.binding_join;
                        }
                    }
                    pass.fixed_bindings = select(state);
                    const bool clobbers = dynamic || transfer;
                    if (clobbers) state = {};
                    last = clobbers ? kGraphInvalidIndex : p;
                }
            }
        }
    }
} // namespace lux::render::detail
