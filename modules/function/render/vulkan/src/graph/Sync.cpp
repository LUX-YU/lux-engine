#include "GraphNative.hpp"
#include <algorithm>

namespace lux::render::vulkan::detail
{
    NativeResourceState useState(const GraphResourceUse& use, std::uint32_t family) noexcept
    {
        NativeResourceState state;
        state.family = family;
        const bool reads = use.access != EGraphAccess::WRITE;
        const bool writes = use.access != EGraphAccess::READ;
        switch (use.usage)
        {
        case EGraphUsage::TRANSFER:
            state.stages = VK_PIPELINE_STAGE_2_COPY_BIT;
            state.access = (reads ? VK_ACCESS_2_TRANSFER_READ_BIT : 0) | (writes ? VK_ACCESS_2_TRANSFER_WRITE_BIT : 0);
            state.layout = writes ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            break;
        case EGraphUsage::COLOR_ATTACHMENT:
        case EGraphUsage::RESOLVE:
            state.stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            state.access = (reads ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : 0) |
                           (writes ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : 0);
            state.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            break;
        case EGraphUsage::DEPTH_ATTACHMENT:
            state.stages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            state.access = (reads ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT : 0) |
                           (writes ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0);
            state.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            break;
        case EGraphUsage::VERTEX:
            state.stages = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
            state.access = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
            break;
        case EGraphUsage::INDEX:
            state.stages = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
            state.access = VK_ACCESS_2_INDEX_READ_BIT;
            break;
        case EGraphUsage::INDIRECT:
            state.stages = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            state.access = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
            break;
        case EGraphUsage::INPUT_ATTACHMENT:
            state.stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            state.access = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
            state.layout = VK_IMAGE_LAYOUT_GENERAL;
            break;
        case EGraphUsage::PRESENT:
            state.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            break;
        default:
            state.stages = ((use.stages & 1) ? VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT : 0) |
                           ((use.stages & 2) ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : 0) |
                           ((use.stages & 4) ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : 0);
            state.access = use.usage == EGraphUsage::UNIFORM ? VK_ACCESS_2_UNIFORM_READ_BIT
                                                             : (reads ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT : 0) |
                                                                   (writes ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0);
            state.layout = VK_IMAGE_LAYOUT_GENERAL;
            break;
        }
        return state;
    }

    bool containsRange(const VGraphRange& parent, const VGraphRange& child) noexcept
    {
        if (const auto* a = std::get_if<BufferRange>(&parent))
        {
            const auto* b = std::get_if<BufferRange>(&child);
            return b && b->byte_offset >= a->byte_offset &&
                   b->byte_offset + b->byte_count <= a->byte_offset + a->byte_count;
        }
        const auto* a = std::get_if<ImageRange>(&parent);
        const auto* b = std::get_if<ImageRange>(&child);
        return a && b &&
               (static_cast<unsigned>(b->aspect) & static_cast<unsigned>(a->aspect)) ==
                   static_cast<unsigned>(b->aspect) &&
               b->base_mip >= a->base_mip && b->base_mip + b->mip_count <= a->base_mip + a->mip_count &&
               b->base_layer >= a->base_layer && b->base_layer + b->layer_count <= a->base_layer + a->layer_count;
    }

    RenderResult<void> compileSync(NativeGraphBacking& result) noexcept
    {
        const auto& graph = result.logical.identity();
        for (std::uint32_t r = 0; r < graph.resources.size(); ++r)
        {
            const auto resource = GraphResourceId{r + 1};
            const auto& declaration = graph.resources[r];
            if (declaration.kind() == EGraphResourceKind::BUFFER)
            {
                std::vector<std::uint64_t> boundaries{0, declaration.buffer().byte_size};
                for (const auto& pass : graph.passes)
                {
                    for (const auto& use : pass.uses)
                    {
                        if (use.resource == resource || (use.fallback && use.fallback->resource == resource))
                        {
                            const auto& range = std::get<BufferRange>(use.range);
                            boundaries.push_back(range.byte_offset);
                            boundaries.push_back(range.byte_offset + range.byte_count);
                        }
                    }
                }
                std::sort(boundaries.begin(), boundaries.end());
                boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
                for (std::size_t i = 1; i < boundaries.size(); ++i)
                {
                    result.cells.push_back({resource, BufferRange{boundaries[i - 1], boundaries[i] - boundaries[i - 1]}}
                    );
                }
            }
            else
            {
                const auto& image = declaration.texture();
                const auto aspects = rdesc::textureAspectMask(image.format);
                for (unsigned aspect : {1u, 2u, 4u})
                {
                    if ((aspect & aspects) == 0)
                    {
                        continue;
                    }
                    for (std::uint32_t mip = 0; mip < image.mip_count; ++mip)
                    {
                        for (std::uint32_t layer = 0; layer < image.array_layers; ++layer)
                        {
                            result.cells.push_back(
                                {resource, ImageRange{static_cast<EAspect>(aspect), mip, 1, layer, 1}}
                            );
                        }
                    }
                }
            }
        }
        for (auto& recipe : result.recipes)
        {
            const auto& pass = graph.passes[recipe.command.pass.value() - 1];
            const auto family = result.queues[static_cast<unsigned>(recipe.command.queue)]->nativeQueue().family;
            // F2 appends proof-only alternative reads after the declaration's uses.
            // Their dependency/version proof remains in LogicalGraphPlan, but the
            // native invocation executes exactly one choice through resourceFor().
            const auto declared_uses =
                result.logical.cacheIdentity().passes[recipe.command.pass.value() - 1].uses.size();
            for (std::uint32_t u = 0; u < declared_uses; ++u)
            {
                const auto& use = pass.uses[u];
                auto resolved_use = use;
                if (use.usage == EGraphUsage::SHADER || use.usage == EGraphUsage::UNIFORM)
                {
                    // Schema defaults may include stages absent from this pipeline.
                    resolved_use.stages &= pass.kind == EPassKind::COMPUTE ? 4u : 3u;
                    if (resolved_use.stages == 0)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument, {recipe.command.pass.value(), u}});
                    }
                }
                UseRecipe access{u, {}, useState(resolved_use, family)};
                if (use.field_index < pass.bindings.size())
                {
                    const auto* field = std::get_if<ShaderResourceBinding>(&pass.bindings[use.field_index].value);
                    if (field && field->role == rdesc::EPassFieldRole::SAMPLED_READ)
                    {
                        access.destination.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                        access.destination.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    }
                }
                // F2 explicitly proves same-pass attachment/local-read pairs.
                // One rendering instance uses one layout and the union of its
                // framebuffer stages/accesses, rather than two conflicting layouts.
                for (const auto& local : pass.uses)
                {
                    if (local.local_read && local.resource == use.resource && local.range == use.range)
                    {
                        for (const auto& paired : pass.uses)
                        {
                            if (paired.resource == use.resource && paired.range == use.range)
                            {
                                const auto state = useState(paired, family);
                                access.destination.stages |= state.stages;
                                access.destination.access |= state.access;
                            }
                        }
                        access.destination.layout = VK_IMAGE_LAYOUT_GENERAL;
                    }
                }
                for (std::uint32_t c = 0; c < result.cells.size(); ++c)
                {
                    const auto& cell = result.cells[c];
                    const bool selected =
                        cell.resource == use.resource || (use.fallback && cell.resource == use.fallback->resource);
                    if (selected && containsRange(use.range, cell.range))
                    {
                        access.cells.push_back(c);
                        ++result.statistics.barriers;
                    }
                }
                // Paired local-read/attachment declarations describe one native
                // cell transition. In particular, never issue two overlapping
                // queue-family releases for that same cell in one submission.
                std::erase_if(
                    access.cells,
                    [&](auto cell)
                    {
                        return std::any_of(
                            recipe.uses.begin(),
                            recipe.uses.end(),
                            [&](const auto& earlier)
                            {
                                return earlier.destination == access.destination &&
                                       std::find(earlier.cells.begin(), earlier.cells.end(), cell) !=
                                           earlier.cells.end();
                            }
                        );
                    }
                );
                recipe.uses.push_back(std::move(access));
            }
        }
        for (std::uint32_t r = 0; r < result.alias_sources.size(); ++r)
        {
            if (!result.alias_sources[r])
            {
                continue;
            }
            VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            bool installed = false;
            for (auto& recipe : result.recipes)
            {
                const auto& pass = graph.passes[recipe.command.pass.value() - 1];
                for (auto& use : recipe.uses)
                {
                    const auto resource = pass.uses[use.use_index].resource;
                    if (resource == *result.alias_sources[r])
                    {
                        memory.srcStageMask |= use.destination.stages;
                        memory.srcAccessMask |= use.destination.access;
                    }
                    if (resource.value() == r + 1 && !installed)
                    {
                        memory.dstStageMask = use.destination.stages;
                        memory.dstAccessMask = use.destination.access;
                        recipe.alias_barriers.push_back(memory);
                        use.alias_source_stages = memory.srcStageMask;
                        installed = true;
                    }
                }
            }
        }
        for (auto& slot : result.slots)
        {
            slot.states.resize(result.cells.size());
            slot.cell_tickets.resize(result.cells.size());
        }
        // Capacity admission includes at most one ownership-release batch per
        // prior queue per pass, plus one completion join. Conditional selection
        // chooses within this cold bound; record never grows command storage.
        std::vector<std::array<bool, 3>> prior_queues(result.cells.size());
        for (std::uint32_t c = 0; c < result.cells.size(); ++c)
        {
            if (result.import_positions[result.cells[c].resource.value() - 1])
            {
                prior_queues[c] = {true, true, true};
            }
        }
        for (const auto& recipe : result.recipes)
        {
            const auto destination = static_cast<unsigned>(recipe.command.queue);
            ++result.admission[destination];
            std::array<bool, 3> releases{};
            for (const auto& use : recipe.uses)
            {
                for (auto cell : use.cells)
                {
                    for (unsigned q = 0; q < 3; ++q)
                    {
                        const bool different_family = result.queues[q]->nativeQueue().family != use.destination.family;
                        releases[q] = releases[q] || (prior_queues[cell][q] && different_family);
                    }
                    prior_queues[cell][destination] = true;
                }
            }
            for (unsigned q = 0; q < 3; ++q)
            {
                result.admission[q] += releases[q] ? 1 : 0;
            }
        }
        ++result.admission[0]; // one final join of all queue last-use tickets
        result.statistics.submissions = result.admission[0] + result.admission[1] + result.admission[2];
        return {};
    }
} // namespace lux::render::vulkan::detail
