#include "GraphNative.hpp"
#include <algorithm>
#include <bit>

namespace lux::render::vulkan::detail
{
    namespace
    {
        VkAttachmentLoadOp loadOp(ELoadOp load) noexcept
        {
            return load == ELoadOp::LOAD    ? VK_ATTACHMENT_LOAD_OP_LOAD
                   : load == ELoadOp::CLEAR ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                            : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        }

        VkAttachmentStoreOp storeOp(EStoreOp store) noexcept
        {
            return store == EStoreOp::STORE ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }

        RenderResult<std::uint32_t> appendView(
            NativeGraphBacking& plan,
            GraphSlot& slot,
            SlotPass& pass,
            GraphResourceId resource,
            ImageRange range,
            VkImageViewType type
        ) noexcept
        {
            const auto* image = imageOf(slot.resources[resource.value() - 1]);
            if (!image)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            auto view = ImageView::create(*image, imageRange(range), type);
            if (!view)
            {
                return cxx::unexpected(view.error());
            }
            SlotPass::ViewPatch patch{resource, {}};
            if (plan.import_positions[resource.value() - 1])
            {
                auto& requirements = plan.import_views[resource.value() - 1];
                const auto native_range = imageRange(range);
                const auto found = std::find_if(
                    requirements.begin(),
                    requirements.end(),
                    [&](const auto& value)
                    {
                        return value.type == type && value.range.aspectMask == native_range.aspectMask &&
                               value.range.baseMipLevel == native_range.baseMipLevel &&
                               value.range.levelCount == native_range.levelCount &&
                               value.range.baseArrayLayer == native_range.baseArrayLayer &&
                               value.range.layerCount == native_range.layerCount;
                    }
                );
                patch.import_view = static_cast<std::uint32_t>(found - requirements.begin());
                if (found == requirements.end())
                {
                    requirements.push_back({native_range, type});
                }
            }
            const auto index = static_cast<std::uint32_t>(pass.views.size());
            pass.views.push_back(std::move(*view));
            pass.view_patches.push_back(patch);
            return index;
        }

        RenderResult<std::vector<std::pair<GraphPassId, std::uint32_t>>> descriptorSources(
            const NativeGraphBacking& result,
            GraphPassId pass_id,
            std::optional<std::uint32_t> program_index
        ) noexcept
        {
            const auto& pass = result.logical.identity().passes[pass_id.value() - 1];
            // Full owner shapes include fields unused by this shader. Resolve
            // them against other candidates carrying the SAME owner identity,
            // once, while all author metadata is available. Never fill a missing
            // declaration with a dummy descriptor or a second resource authority.
            std::vector<std::pair<GraphPassId, std::uint32_t>> fields;
            for (std::uint32_t f = 0; f < pass.bindings.size(); ++f)
            {
                fields.emplace_back(pass_id, f);
            }
            if (program_index)
            {
                const auto& layout = result.programs[*program_index].identity().layout;
                for (const auto& owner : layout.owners)
                {
                    for (const auto& shape : owner.fields)
                    {
                        for (std::uint32_t element = 0; element < shape.count; ++element)
                        {
                            const auto matches = [&](const GraphFieldBinding& field)
                            {
                                const std::string_view semantic = field.semantic.empty() ? field.path : field.semantic;
                                return field.owner == owner.category && semantic == shape.semantic &&
                                       field.array_element == element;
                            };
                            if (std::any_of(pass.bindings.begin(), pass.bindings.end(), matches))
                            {
                                continue;
                            }
                            std::optional<std::pair<GraphPassId, std::uint32_t>> source;
                            for (const auto& candidate : result.programs)
                            {
                                const auto& key = candidate.identity();
                                const auto shared =
                                    std::find(key.layout.owners.begin(), key.layout.owners.end(), owner);
                                if (shared == key.layout.owners.end())
                                {
                                    continue;
                                }
                                const auto& bindings = result.logical.identity().passes[key.pass.value() - 1].bindings;
                                for (std::uint32_t f = 0; f < bindings.size(); ++f)
                                {
                                    if (!matches(bindings[f]))
                                    {
                                        continue;
                                    }
                                    if (source)
                                    {
                                        const auto& previous = result.logical.identity()
                                                                   .passes[source->first.value() - 1]
                                                                   .bindings[source->second];
                                        if (previous.value != bindings[f].value)
                                        {
                                            return cxx::unexpected(RenderError{kInvalidArgument});
                                        }
                                    }
                                    else
                                    {
                                        source = {key.pass, f};
                                    }
                                }
                            }
                            if (!source)
                            {
                                return cxx::unexpected(RenderError{kInvalidArgument});
                            }
                            fields.push_back(*source);
                        }
                    }
                }
            }
            return fields;
        }

        RenderResult<void> assemblePass(
            NativeGraphBacking& result,
            const NativeCompileInputs& inputs,
            const NativePassRecipe& recipe,
            GraphSlot& slot
        ) noexcept
        {
            const auto index = recipe.command.pass.value() - 1;
            const auto& pass = result.logical.identity().passes[index];
            auto& output = slot.passes[index];
            auto captured = descriptorSources(result, recipe.command.pass, recipe.program);
            if (!captured)
            {
                return cxx::unexpected(captured.error());
            }
            const auto& fields = *captured;
            output.views.reserve(fields.size() * 2);
            std::vector<OwnerDescriptorValue> values;
            for (const auto [source_pass, f] : fields)
            {
                const auto& source = result.logical.identity().passes[source_pass.value() - 1];
                const auto& source_values = inputs.initial_values.passes[source_pass.value() - 1];
                const auto& field = source.bindings[f];
                const auto* attachment = std::get_if<AttachmentBinding>(&field.value);
                if (attachment)
                {
                    const auto* image = imageOf(slot.resources[attachment->resource.value() - 1]);
                    if (!image || attachment->range.mip_count != 1)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                    auto view_index = appendView(
                        result,
                        slot,
                        output,
                        attachment->resource,
                        attachment->range,
                        attachment->range.layer_count == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY
                    );
                    if (!view_index)
                    {
                        return cxx::unexpected(view_index.error());
                    }
                    const auto* view = &output.views[*view_index];
                    const auto extent = view->extent();
                    const auto& graphics =
                        std::get<GraphicsDescription>(result.programs[*recipe.program].identity().pipeline);
                    if (attachment->range.layer_count < std::max(1, std::bit_width(graphics.view_mask)))
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                    const bool wrong_extent = output.extent.width != 0 && (output.extent.width != extent.width ||
                                                                           output.extent.height != extent.height);
                    if (wrong_extent)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                    output.extent = extent;
                    AttachmentRecipe bound{f, *view_index, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
                    bound.native.imageView = view->native();
                    bound.native.imageLayout = attachment->role != rdesc::EPassFieldRole::DEPTH_STENCIL
                                                   ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                   : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    const bool local_pair = std::any_of(
                        pass.uses.begin(),
                        pass.uses.end(),
                        [&](const auto& use)
                        {
                            return use.local_read && use.resource == attachment->resource &&
                                   use.range == VGraphRange{attachment->range};
                        }
                    );
                    if (local_pair)
                    {
                        if (!inputs.device.localRead())
                        {
                            return cxx::unexpected(RenderError{kUnsupported});
                        }
                        bound.native.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    }
                    bound.native.loadOp = loadOp(attachment->load);
                    bound.native.storeOp = storeOp(attachment->store);
                    if (attachment->role == rdesc::EPassFieldRole::COLOR_ATTACHMENT)
                    {
                        output.colors.push_back(bound);
                    }
                    else if (attachment->role == rdesc::EPassFieldRole::DEPTH_STENCIL)
                    {
                        const auto aspects = static_cast<std::uint32_t>(attachment->range.aspect);
                        if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
                        {
                            output.depth = bound;
                        }
                        if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0)
                        {
                            bound.native.loadOp = loadOp(attachment->stencil_load);
                            bound.native.storeOp = storeOp(attachment->stencil_store);
                            output.stencil = bound;
                        }
                    }
                    continue;
                }
                if (!recipe.program || std::holds_alternative<TransferBinding>(field.value))
                {
                    continue;
                }
                const auto& program = result.programs[*recipe.program];
                const auto& layout = program.identity().layout;
                const auto owner = std::find_if(
                    layout.owners.begin(),
                    layout.owners.end(),
                    [&](const auto& shape) { return shape.category == field.owner; }
                );
                if (owner == layout.owners.end())
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const std::string_view semantic = field.semantic.empty() ? field.path : field.semantic;
                SlotPass::DescriptorPatch patch{source_pass, f, {}, {}, {}};
                if (std::holds_alternative<SamplerBinding>(field.value))
                {
                    const auto* sampler =
                        f < source_values.fields.size() ? std::get_if<GraphSampler>(&source_values.fields[f]) : nullptr;
                    if (!sampler)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                    const auto native = std::find_if(
                        inputs.samplers.begin(),
                        inputs.samplers.end(),
                        [&](const auto& value) { return value.first == *sampler; }
                    );
                    if (native == inputs.samplers.end())
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                    values.push_back({owner->identity, semantic, field.array_element, native->second});
                }
                else if (auto* resource = std::get_if<ShaderResourceBinding>(&field.value))
                {
                    const auto use = std::find_if(
                        source.uses.begin(),
                        source.uses.end(),
                        [&](const auto& value) { return value.field_index == f; }
                    );
                    if (use == source.uses.end())
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                    patch.use = static_cast<std::uint32_t>(use - source.uses.begin());
                    const auto& backing = slot.resources[resource->resource.value() - 1];
                    if (auto* image = imageOf(backing))
                    {
                        const auto* range = std::get_if<ImageRange>(&resource->range);
                        if (!range)
                        {
                            return cxx::unexpected(RenderError{kInvalidArgument});
                        }
                        auto view = appendView(
                            result,
                            slot,
                            output,
                            resource->resource,
                            *range,
                            resource->dimension.ends_with("Array") ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D
                        );
                        if (!view)
                        {
                            return cxx::unexpected(view.error());
                        }
                        patch.primary_view = *view;
                        if (use->fallback)
                        {
                            auto fallback = appendView(
                                result,
                                slot,
                                output,
                                use->fallback->resource,
                                *range,
                                resource->dimension.ends_with("Array") ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                                       : VK_IMAGE_VIEW_TYPE_2D
                            );
                            if (!fallback)
                            {
                                return cxx::unexpected(fallback.error());
                            }
                            patch.fallback_view = *fallback;
                        }
                        const auto image_layout = resource->role == rdesc::EPassFieldRole::SAMPLED_READ
                                                      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                      : VK_IMAGE_LAYOUT_GENERAL;
                        values.push_back(
                            {owner->identity,
                             semantic,
                             field.array_element,
                             ImageDescriptorValue{std::cref(output.views[*view]), image_layout}}
                        );
                    }
                    else
                    {
                        const auto* buffer = bufferOf(backing);
                        const auto* range = std::get_if<BufferRange>(&resource->range);
                        if (!buffer || !range)
                        {
                            return cxx::unexpected(RenderError{kInvalidArgument});
                        }
                        values.push_back(
                            {owner->identity,
                             semantic,
                             field.array_element,
                             BufferDescriptorValue{std::cref(*buffer), range->byte_offset, range->byte_count}}
                        );
                    }
                }
                output.descriptor_patches.push_back(patch);
            }
            // Resolve association is an authored field path, resolved only at compilation.
            for (std::uint32_t f = 0; f < pass.bindings.size(); ++f)
            {
                const auto* resolve = std::get_if<AttachmentBinding>(&pass.bindings[f].value);
                if (!resolve || resolve->role != rdesc::EPassFieldRole::RESOLVE)
                {
                    continue;
                }
                const auto source = std::find_if(
                    output.colors.begin(),
                    output.colors.end(),
                    [&](const auto& value) { return pass.bindings[value.field].path == resolve->paired_texture; }
                );
                if (source == output.colors.end())
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                // All attachment views precede the following descriptor views only by field order.
                // Locate the unique image/range instead of assuming adjacent fields or equal indices.
                const auto* image = imageOf(slot.resources[resolve->resource.value() - 1]);
                const auto view = std::find_if(
                    output.views.begin(),
                    output.views.end(),
                    [&](const auto& value)
                    {
                        const auto range = imageRange(resolve->range);
                        return value.image() == image->native() && value.range().baseMipLevel == range.baseMipLevel &&
                               value.range().baseArrayLayer == range.baseArrayLayer &&
                               value.range().layerCount == range.layerCount;
                    }
                );
                if (view == output.views.end())
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const auto clear_class = rdesc::textureClearClass(
                    result.logical.identity().resources[resolve->resource.value() - 1].texture().format
                );
                source->native.resolveMode = clear_class == rdesc::ETextureClearClass::FLOAT
                                                 ? VK_RESOLVE_MODE_AVERAGE_BIT
                                                 : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                source->native.resolveImageView = view->native();
                source->resolve_view = static_cast<std::uint32_t>(view - output.views.begin());
                source->native.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            output.color_scratch.resize(output.colors.size());
            for (const auto& copy : recipe.copies)
            {
                if (!copy.destination_use)
                {
                    auto readback = Buffer::create(
                        inputs.allocator,
                        copy.bytes,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        EMemoryAccess::READBACK
                    );
                    if (!readback)
                    {
                        return cxx::unexpected(readback.error());
                    }
                    ++result.statistics.allocations;
                    result.statistics.allocated_bytes += readback->allocationBytes();
                    output.readbacks.push_back(std::move(*readback));
                }
            }
            for (const auto& view : output.views)
            {
                output.current_views.push_back(&view);
            }
            if (recipe.program)
            {
                const auto& program = result.programs[*recipe.program];
                auto descriptors =
                    BoundDescriptorSets::create(inputs.device, program, values, EDescriptorUpdates::COMPLETED_ONLY);
                if (!descriptors)
                {
                    return cxx::unexpected(descriptors.error());
                }
                output.descriptors.emplace(std::move(*descriptors));
                for (const auto& value : values)
                {
                    output.descriptor_values.push_back(value.value);
                }
                output.dynamic_offsets.resize(program.identity().layout.dynamic_count);
            }
            return {};
        }
    } // namespace

    RenderResult<void> compileRecipes(NativeGraphBacking& result, const NativeCompileInputs& inputs) noexcept
    {
        if (inputs.device.localRead())
        {
            result.set_input_indices = reinterpret_cast<PFN_vkCmdSetRenderingInputAttachmentIndicesKHR>(
                vkGetDeviceProcAddr(result.device, "vkCmdSetRenderingInputAttachmentIndicesKHR")
            );
            if (!result.set_input_indices)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
        }
        const auto& graph = result.logical.identity();
        if (inputs.commands.size() != result.logical.executionOrder().size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        for (const auto pass : result.logical.executionOrder())
        {
            const auto matches = [&](const auto& command) { return command.pass == pass; };
            if (std::count_if(inputs.commands.begin(), inputs.commands.end(), matches) != 1)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto& command = *std::find_if(inputs.commands.begin(), inputs.commands.end(), matches);
            const auto kind = graph.passes[pass.value() - 1].kind;
            const bool wrong_command =
                (kind == EPassKind::GRAPHICS && !std::holds_alternative<DrawCommand>(command.command)) ||
                (kind == EPassKind::COMPUTE && !std::holds_alternative<DispatchCommand>(command.command)) ||
                ((kind == EPassKind::TRANSFER || kind == EPassKind::HOST_READBACK) &&
                 !std::holds_alternative<CopyCommand>(command.command));
            if (wrong_command || static_cast<unsigned>(command.queue) > 2)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto flags = result.queues[static_cast<unsigned>(command.queue)]->nativeQueue().flags;
            const bool unsupported = (kind == EPassKind::GRAPHICS && (flags & VK_QUEUE_GRAPHICS_BIT) == 0) ||
                                     (kind == EPassKind::COMPUTE && (flags & VK_QUEUE_COMPUTE_BIT) == 0);
            if (unsupported)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            if (const auto* dispatch = std::get_if<DispatchCommand>(&command.command))
            {
                const auto& maximum = inputs.device.properties().limits.maxComputeWorkGroupCount;
                if (dispatch->x > maximum[0] || dispatch->y > maximum[1] || dispatch->z > maximum[2])
                {
                    return cxx::unexpected(RenderError{kInvalidArgument, {pass.value()}});
                }
            }
            const auto& declaration = graph.passes[pass.value() - 1];
            const bool transfer_without_graphics =
                std::holds_alternative<CopyCommand>(command.command) && (flags & VK_QUEUE_GRAPHICS_BIT) == 0;
            for (const auto& use : declaration.uses)
            {
                const auto* image = std::get_if<ImageRange>(&use.range);
                if (transfer_without_graphics && image && image->aspect != EAspect::COLOR)
                {
                    // Device's newer depth/stencil transfer capabilities are not
                    // enabled by R4; use the graphics queue for this native copy.
                    return cxx::unexpected(RenderError{kUnsupported, {pass.value()}});
                }
            }
            NativePassRecipe recipe{command};
            if (std::holds_alternative<CopyCommand>(command.command))
            {
                auto copies = compileCopies(recipe, result.logical);
                if (!copies)
                {
                    return cxx::unexpected(copies.error());
                }
            }
            for (std::uint32_t p = 0; p < result.programs.size(); ++p)
            {
                if (result.programs[p].identity().pass == pass)
                {
                    recipe.program = p;
                }
            }
            if (recipe.program && kind == EPassKind::GRAPHICS)
            {
                const auto& graphics =
                    std::get<GraphicsDescription>(result.programs[*recipe.program].identity().pipeline);
                if (!graphics.vertex_bindings.empty())
                {
                    // Direct procedural draw recipes have no vertex-stream mapping.
                    return cxx::unexpected(RenderError{kUnsupported, {pass.value()}});
                }
            }
            for (const auto& edge : result.logical.dependencies())
            {
                if (edge.after == pass)
                {
                    recipe.predecessors.push_back(edge.before.value() - 1);
                }
            }
            result.recipes.push_back(std::move(recipe));
        }
        for (auto& slot : result.slots)
        {
            for (const auto& recipe : result.recipes)
            {
                auto assembled = assemblePass(result, inputs, recipe, slot);
                if (!assembled)
                {
                    return cxx::unexpected(assembled.error());
                }
            }
        }
        return {};
    }
} // namespace lux::render::vulkan::detail
