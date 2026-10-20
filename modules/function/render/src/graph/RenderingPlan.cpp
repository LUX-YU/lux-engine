#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/graph/RenderingPlan.hpp>

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

        bool attachmentTransition(const ImageTransition& transition, const RasterCommands& work) noexcept
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
        recording.groups.clear();
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            auto& pass = recording.passes[p];
            auto* raster = std::get_if<RasterCommands>(&pass.work);
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
                return std::ranges::none_of(
                    analysis.versions,
                    [&](const auto& use)
                    {
                        return use.version == attachment.access.version &&
                               use.producer == analysis.execution_order[p] &&
                               detail::overlaps(use.range, attachment.access.range);
                    }
                );
            };
            const bool invalid_store = !std::ranges::all_of(raster->colors, validateStore) ||
                                       (raster->depth && !validateStore(*raster->depth));
            if (invalid_store)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }

            const auto* previous = p ? std::get_if<RasterCommands>(&recording.passes[p - 1].work) : nullptr;
            bool same_attachments = previous && previous->colors.size() == raster->colors.size() &&
                                    bool(previous->depth) == bool(raster->depth);
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
                pass.before.buffers.empty() &&
                std::ranges::all_of(
                    pass.before.images,
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
                pass.before.images.clear();
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
                const bool needed = std::ranges::any_of(
                    analysis.lifetimes,
                    [&](const auto& use)
                    {
                        return use.version.resource == resource && use.last > end &&
                               detail::overlaps(use.range, attachment.access.range);
                    }
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
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            const auto& batch = recording.passes[p].before;
            for (std::uint32_t i = 0; i < batch.images.size(); ++i)
            {
                resources[batch.images[i].resource].barrier_uses.emplace_back(p, i);
            }
            for (std::uint32_t i = 0; i < batch.buffers.size(); ++i)
            {
                resources[batch.buffers[i].resource].barrier_uses.emplace_back(p, i);
            }
        }
        return {};
    }
} // namespace lux::render::detail
