#include "GraphNative.hpp"
#include <algorithm>
#include <lux/engine/render/vulkan/shader/Format.hpp>

namespace lux::render::vulkan::detail
{
    NativeRangeState importState(const NativeImportBinding& input, const ResourceCell& cell) noexcept
    {
        if (input.ranges.empty())
        {
            auto state = input.initial;
            if (input.ready && state.family == VK_QUEUE_FAMILY_IGNORED)
            {
                state.family = input.ready->owner().nativeQueue().family;
            }
            return {cell.range, state, input.ready};
        }
        const auto found = std::find_if(
            input.ranges.begin(),
            input.ranges.end(),
            [&](const auto& value) { return containsRange(value.range, cell.range); }
        );
        if (found == input.ranges.end())
        {
            std::terminate(); // prepareSlot validates complete, unambiguous coverage.
        }
        auto result = *found;
        if (result.ready && result.state.family == VK_QUEUE_FAMILY_IGNORED)
        {
            result.state.family = result.ready->owner().nativeQueue().family;
        }
        return result;
    }

    static RenderResult<void> validateImports(
        const NativeGraphBacking& plan,
        const FrameGraphBindings& bindings,
        std::span<const NativeImportBinding> imports
    ) noexcept
    {
        const auto& graph = plan.logical.identity();
        for (std::uint32_t i = 0; i < imports.size(); ++i)
        {
            const auto& input = imports[i];
            const auto& logical = bindings.imports()[i];
            const bool wrong_identity = input.resource != plan.imports[i].resource ||
                                        input.identity != logical.backing ||
                                        input.backing.index() != plan.imports[i].backing.index();
            if (wrong_identity || (!input.host_ready && !input.ready && input.ranges.empty()))
            {
                return cxx::unexpected(RenderError{kInvalidArgument, {i}});
            }
            const auto r = input.resource.value() - 1;
            const auto& declaration = graph.resources[r];
            for (std::uint32_t other = 0; other < i; ++other)
            {
                const bool same_native = std::visit(
                    [&](const auto& owner)
                    {
                        const auto* previous =
                            std::get_if<std::remove_cvref_t<decltype(owner)>>(&imports[other].backing);
                        return previous && previous->get().native() == owner.get().native();
                    },
                    input.backing
                );
                if (same_native)
                {
                    // Distinct logical identities cannot secretly alias imported
                    // native storage outside the compiler's alias proof.
                    return cxx::unexpected(RenderError{kInvalidArgument, {i, other}});
                }
            }
            for (std::uint32_t c = 0; c < plan.cells.size(); ++c)
            {
                if (plan.cells[c].resource != input.resource)
                {
                    continue;
                }
                const auto& cell = plan.cells[c];
                if (!input.ranges.empty() &&
                    std::count_if(
                        input.ranges.begin(),
                        input.ranges.end(),
                        [&](const auto& state) { return containsRange(state.range, cell.range); }
                    ) != 1)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument, {i, c}});
                }
                const auto supplied = importState(input, cell);
                if (!supplied.ready && !input.host_ready)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument, {i, c}});
                }
                if (supplied.ready)
                {
                    const auto& owner = supplied.ready->owner();
                    const bool wrong_ticket =
                        owner.device() != plan.device || !owner.owns(*supplied.ready) ||
                        std::find(plan.queues.begin(), plan.queues.end(), &owner) == plan.queues.end() ||
                        (supplied.state.family != VK_QUEUE_FAMILY_IGNORED &&
                         supplied.state.family != owner.nativeQueue().family);
                    if (wrong_ticket)
                    {
                        return cxx::unexpected(RenderError{kWrongOwner, {i, c}});
                    }
                }
                const auto first = std::find_if(
                    plan.recipes.begin(),
                    plan.recipes.end(),
                    [&](const auto& recipe)
                    {
                        if (!bindings.invocation(recipe.command.pass)->enabled)
                        {
                            return false;
                        }
                        return std::any_of(
                            recipe.uses.begin(),
                            recipe.uses.end(),
                            [&](const auto& use)
                            {
                                return bindings.resourceFor(recipe.command.pass, use.use_index) == input.resource &&
                                       std::find(use.cells.begin(), use.cells.end(), c) != use.cells.end();
                            }
                        );
                    }
                );
                if (first == plan.recipes.end())
                {
                    continue;
                }
                if (!supplied.ready && supplied.state.family != VK_QUEUE_FAMILY_IGNORED &&
                    plan.queues[static_cast<unsigned>(first->command.queue)]->nativeQueue().family !=
                        supplied.state.family)
                {
                    return cxx::unexpected(RenderError{kWrongOwner, {i}});
                }
                const auto use = std::find_if(
                    first->uses.begin(),
                    first->uses.end(),
                    [&](const auto& use) { return std::find(use.cells.begin(), use.cells.end(), c) != use.cells.end(); }
                );
                const auto access = graph.passes[first->command.pass.value() - 1].uses[use->use_index].access;
                if (declaration.kind() == EGraphResourceKind::IMAGE && access != EGraphAccess::WRITE &&
                    supplied.state.layout == VK_IMAGE_LAYOUT_UNDEFINED)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument, {i, c}});
                }
                for (const auto& previous : plan.slots)
                {
                    for (std::uint32_t previous_cell = 0; previous_cell < plan.cells.size(); ++previous_cell)
                    {
                        const auto previous_resource = plan.cells[previous_cell].resource.value() - 1;
                        const auto& ticket = previous.cell_tickets[previous_cell];
                        if (!ticket || ticket->serial() <= ticket->owner().completed())
                        {
                            continue;
                        }
                        const bool same_backing = std::visit(
                            [&](const auto& owner)
                            {
                                using T = std::remove_cvref_t<decltype(owner.get())>;
                                if constexpr (std::same_as<T, Buffer>)
                                {
                                    const auto* buffer = bufferOf(previous.resources[previous_resource]);
                                    return buffer && buffer->native() == owner.get().native();
                                }
                                else
                                {
                                    const auto* image = imageOf(previous.resources[previous_resource]);
                                    return image && image->native() == owner.get().native();
                                }
                            },
                            input.backing
                        );
                        const bool ordered = supplied.ready && &supplied.ready->owner() == &ticket->owner() &&
                                             supplied.ready->serial() >= ticket->serial();
                        if (same_backing && !ordered)
                        {
                            return cxx::unexpected(RenderError{kBusy, {i, c}});
                        }
                    }
                }
            }
            if (const auto* buffer = std::get_if<std::reference_wrapper<const Buffer>>(&input.backing))
            {
                const auto& actual = buffer->get();
                const bool invalid = actual.device() != plan.device || !actual.native() ||
                                     (actual.usage() & plan.resource_usage[r]) != plan.resource_usage[r] ||
                                     logical.dynamic_offset > actual.size() ||
                                     declaration.buffer().byte_size > actual.size() - logical.dynamic_offset;
                if (invalid)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument, {i}});
                }
            }
            else
            {
                const auto& image = std::get<std::reference_wrapper<const Image>>(input.backing).get();
                const auto& actual = image.description();
                const auto& expected = declaration.texture();
                const bool invalid =
                    image.device() != plan.device || !image.native() ||
                    (actual.usage & plan.resource_usage[r]) != plan.resource_usage[r] ||
                    actual.format != nativeTextureFormat(expected.format) || actual.extent.width != expected.width ||
                    actual.extent.height != expected.height || actual.mip_levels != expected.mip_count ||
                    actual.array_layers != expected.array_layers || actual.samples != expected.samples ||
                    (!input.views.empty() && input.views.size() != plan.import_views[r].size());
                if (invalid)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument, {i}});
                }
                for (std::size_t v = 0; v < input.views.size(); ++v)
                {
                    const auto& view = input.views[v].get();
                    const auto& shape = plan.import_views[r][v];
                    const auto& range = view.range();
                    const bool mismatch = view.image() != image.native() || view.type() != shape.type ||
                                          range.aspectMask != shape.range.aspectMask ||
                                          range.baseMipLevel != shape.range.baseMipLevel ||
                                          range.levelCount != shape.range.levelCount ||
                                          range.baseArrayLayer != shape.range.baseArrayLayer ||
                                          range.layerCount != shape.range.layerCount;
                    if (mismatch)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument, {i, v}});
                    }
                }
            }
        }
        return {};
    }

    RenderResult<void> prepareSlot(
        NativeGraphBacking& plan,
        GraphSlot& slot,
        const FrameGraphBindings& bindings,
        std::span<const NativeImportBinding> imports
    ) noexcept
    {
        auto valid = validateImports(plan, bindings, imports);
        if (!valid)
        {
            return cxx::unexpected(valid.error());
        }
        const auto& graph = plan.logical.identity();
        std::fill(slot.buffer_offsets.begin(), slot.buffer_offsets.end(), 0);
        for (std::uint32_t i = 0; i < imports.size(); ++i)
        {
            const auto r = imports[i].resource.value() - 1;
            slot.buffer_offsets[r] = bindings.imports()[i].dynamic_offset;
            std::visit([&](const auto& backing) { slot.resources[r] = backing; }, imports[i].backing);
        }
        for (const auto& recipe : plan.recipes)
        {
            const auto p = recipe.command.pass.value() - 1;
            auto& pass = slot.passes[p];
            for (const auto& copy : recipe.copies)
            {
                const auto source = bindings.resourceFor(recipe.command.pass, copy.source_use).value() - 1;
                const auto destination =
                    copy.destination_use ? bindings.resourceFor(recipe.command.pass, *copy.destination_use).value() - 1
                                         : source;
                const bool misaligned = slot.buffer_offsets[source] % copy.buffer_alignment != 0 ||
                                        slot.buffer_offsets[destination] % copy.buffer_alignment != 0;
                if (misaligned)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument, {p + 1}});
                }
            }
            for (std::uint32_t v = 0; v < pass.views.size(); ++v)
            {
                const auto& patch = pass.view_patches[v];
                const auto r = patch.resource.value() - 1;
                pass.current_views[v] = &pass.views[v];
                if (patch.import_view)
                {
                    const auto& input = imports[*plan.import_positions[r]];
                    if (!input.views.empty())
                    {
                        pass.current_views[v] = &input.views[*patch.import_view].get();
                    }
                    else if (pass.views[v].image() != imageOf(slot.resources[r])->native())
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument, {r + 1, v}});
                    }
                }
            }
            const auto& invocation = *bindings.invocation(recipe.command.pass);
            if (!invocation.enabled)
            {
                continue;
            }
            for (std::uint32_t d = 0; d < pass.descriptor_patches.size(); ++d)
            {
                const auto& patch = pass.descriptor_patches[d];
                auto& value = pass.descriptor_values[d];
                if (!patch.use)
                {
                    const auto sampler =
                        std::get<GraphSampler>(bindings.invocation(patch.source_pass)->fields[patch.field]);
                    const auto choice = std::find_if(
                        plan.samplers.begin(),
                        plan.samplers.end(),
                        [&](const auto& known) { return known.first == sampler; }
                    );
                    if (choice == plan.samplers.end())
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument, {p + 1, patch.field}});
                    }
                    value = choice->second;
                    continue;
                }
                const auto& use = graph.passes[patch.source_pass.value() - 1].uses[*patch.use];
                const auto selected = bindings.resourceFor(patch.source_pass, *patch.use);
                if (patch.primary_view)
                {
                    const auto view = selected == use.resource ? *patch.primary_view : *patch.fallback_view;
                    std::get<ImageDescriptorValue>(value).image = std::cref(*pass.current_views[view]);
                }
                else
                {
                    const auto range = std::get<BufferRange>(use.range);
                    value = BufferDescriptorValue{
                        std::cref(*bufferOf(slot.resources[selected.value() - 1])),
                        range.byte_offset + slot.buffer_offsets[selected.value() - 1],
                        range.byte_count
                    };
                }
            }
            if (pass.descriptors)
            {
                const auto previous = slot.completion ? std::span<const SubmissionTicket>{&*slot.completion, 1}
                                                      : std::span<const SubmissionTicket>{};
                auto rewritten = pass.descriptors->rewrite(pass.descriptor_values, previous);
                if (!rewritten)
                {
                    return cxx::unexpected(rewritten.error());
                }
            }
        }
        return {};
    }
} // namespace lux::render::vulkan::detail
