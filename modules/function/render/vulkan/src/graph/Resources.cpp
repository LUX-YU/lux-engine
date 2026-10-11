#include "GraphNative.hpp"
#include <algorithm>
#include <lux/engine/render/vulkan/shader/Format.hpp>

namespace lux::render::vulkan::detail
{
    const Buffer* bufferOf(const VResourceBacking& resource) noexcept
    {
        if (auto* owned = std::get_if<Buffer>(&resource))
        {
            return owned;
        }
        auto* borrowed = std::get_if<std::reference_wrapper<const Buffer>>(&resource);
        return borrowed ? &borrowed->get() : nullptr;
    }

    const Image* imageOf(const VResourceBacking& resource) noexcept
    {
        if (auto* owned = std::get_if<Image>(&resource))
        {
            return owned;
        }
        auto* borrowed = std::get_if<std::reference_wrapper<const Image>>(&resource);
        return borrowed ? &borrowed->get() : nullptr;
    }

    VkImageSubresourceRange imageRange(const ImageRange& range) noexcept
    {
        return {
            static_cast<VkImageAspectFlags>(range.aspect),
            range.base_mip,
            range.mip_count,
            range.base_layer,
            range.layer_count
        };
    }

    namespace
    {
        // Conservative physical compatibility: identical image shape and usage,
        // no export/history, and all uses on the same actual queue. This is a
        // proved subset, not a promise that logical intervals imply GPU safety.
        bool orderedAlias(const NativeCompileInputs& inputs, GraphResourceId earlier, GraphResourceId later) noexcept
        {
            const auto& graph = inputs.logical.identity();
            const auto& a = graph.resources[earlier.value() - 1];
            const auto& b = graph.resources[later.value() - 1];
            const bool incompatible = a.origin != EGraphResourceOrigin::TRANSIENT ||
                                      b.origin != EGraphResourceOrigin::TRANSIENT || a.description != b.description;
            if (incompatible)
            {
                return false;
            }
            if (std::any_of(
                    graph.outputs.begin(),
                    graph.outputs.end(),
                    [&](const auto& output) { return output.resource == earlier || output.resource == later; }
                ))
            {
                return false;
            }
            VkQueue common = VK_NULL_HANDLE;
            bool observed_a = false, observed_b = false;
            for (auto pass_id : inputs.logical.executionOrder())
            {
                const auto& pass = graph.passes[pass_id.value() - 1];
                bool uses_a = false, uses_b = false;
                for (const auto& use : pass.uses)
                {
                    uses_a = uses_a || use.resource == earlier || (use.fallback && use.fallback->resource == earlier);
                    uses_b = uses_b || use.resource == later || (use.fallback && use.fallback->resource == later);
                }
                if (!uses_a && !uses_b)
                {
                    continue;
                }
                if (pass.condition.isValid() || (uses_a && (uses_b || observed_b)))
                {
                    return false;
                }
                const auto command = std::find_if(
                    inputs.commands.begin(),
                    inputs.commands.end(),
                    [&](const auto& value) { return value.pass == pass_id; }
                );
                if (command == inputs.commands.end() || static_cast<unsigned>(command->queue) > 2)
                {
                    return false;
                }
                const auto queue = inputs.queues[static_cast<unsigned>(command->queue)]->nativeQueue().handle;
                if (common && common != queue)
                {
                    return false;
                }
                common = queue;
                observed_a = observed_a || uses_a;
                observed_b = observed_b || uses_b;
            }
            return observed_a && observed_b;
        }

        std::uint32_t resourceUsage(const LogicalGraphPlan& plan, GraphResourceId resource, bool image) noexcept
        {
            const auto& graph = plan.identity();
            std::uint32_t result = 0;
            for (const auto id : plan.executionOrder())
            {
                const auto& pass = graph.passes[id.value() - 1];
                const auto declared = plan.cacheIdentity().passes[id.value() - 1].uses.size();
                for (std::uint32_t u = 0; u < declared; ++u)
                {
                    const auto& use = pass.uses[u];
                    if (use.resource != resource && (!use.fallback || use.fallback->resource != resource))
                    {
                        continue;
                    }
                    if (image)
                    {
                        switch (use.usage)
                        {
                        case EGraphUsage::TRANSFER:
                            result |= use.access == EGraphAccess::READ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                                                       : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                            break;
                        case EGraphUsage::COLOR_ATTACHMENT:
                        case EGraphUsage::RESOLVE:
                            result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                            break;
                        case EGraphUsage::DEPTH_ATTACHMENT:
                            result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                            break;
                        case EGraphUsage::INPUT_ATTACHMENT:
                            result |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
                            break;
                        case EGraphUsage::SHADER:
                            // Read-only storage uses are distinguished by their generated role.
                            if (use.field_index < pass.bindings.size())
                            {
                                auto* field = std::get_if<ShaderResourceBinding>(&pass.bindings[use.field_index].value);
                                result |= field && field->role == rdesc::EPassFieldRole::SAMPLED_READ
                                              ? VK_IMAGE_USAGE_SAMPLED_BIT
                                              : VK_IMAGE_USAGE_STORAGE_BIT;
                            }
                            else
                            {
                                result |= VK_IMAGE_USAGE_STORAGE_BIT;
                            }
                            break;
                        default:
                            break;
                        }
                    }
                    else
                    {
                        switch (use.usage)
                        {
                        case EGraphUsage::TRANSFER:
                            result |= use.access == EGraphAccess::READ ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                                                       : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                            break;
                        case EGraphUsage::UNIFORM:
                            result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                            break;
                        case EGraphUsage::VERTEX:
                            result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
                            break;
                        case EGraphUsage::INDEX:
                            result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
                            break;
                        case EGraphUsage::INDIRECT:
                            result |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
                            break;
                        case EGraphUsage::SHADER:
                            result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                            break;
                        default:
                            break;
                        }
                    }
                }
            }
            return result;
        }
    } // namespace

    RenderResult<void> compileResources(NativeGraphBacking& result, const NativeCompileInputs& inputs) noexcept
    {
        const auto& graph = result.logical.identity();
        if (inputs.imports.size() != result.logical.imports().size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        result.slots.reserve(inputs.frame_capacity);
        result.import_positions.resize(graph.resources.size());
        result.import_views.resize(graph.resources.size());
        result.resource_usage.resize(graph.resources.size());
        for (std::uint32_t i = 0; i < inputs.imports.size(); ++i)
        {
            if (inputs.imports[i].resource != result.logical.imports()[i])
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            result.import_positions[inputs.imports[i].resource.value() - 1] = i;
        }
        result.alias_sources.resize(graph.resources.size());
        if (inputs.alias)
        {
            for (std::uint32_t b = 0; b < graph.resources.size(); ++b)
            {
                if (graph.resources[b].kind() != EGraphResourceKind::IMAGE)
                {
                    continue;
                }
                for (std::uint32_t a = 0; a < b; ++a)
                {
                    const auto first = GraphResourceId{a + 1}, second = GraphResourceId{b + 1};
                    const bool equal_usage =
                        resourceUsage(inputs.logical, first, true) == resourceUsage(inputs.logical, second, true);
                    const bool already_reused = std::any_of(
                        result.alias_sources.begin(),
                        result.alias_sources.end(),
                        [&](const auto& source) { return source && *source == first; }
                    );
                    if (equal_usage && !already_reused && !result.alias_sources[a] &&
                        orderedAlias(inputs, first, second))
                    {
                        result.alias_sources[b] = first;
                        break;
                    }
                }
            }
        }
        for (std::uint32_t slot_index = 0; slot_index < inputs.frame_capacity; ++slot_index)
        {
            auto& slot = result.slots.emplace_back();
            slot.resources.reserve(graph.resources.size());
            slot.passes.resize(graph.passes.size());
            slot.pass_tickets.resize(graph.passes.size());
            slot.buffer_offsets.resize(graph.resources.size());
            for (std::uint32_t r = 0; r < graph.resources.size(); ++r)
            {
                const auto id = GraphResourceId{r + 1};
                const auto& declaration = graph.resources[r];
                const bool is_image = declaration.kind() == EGraphResourceKind::IMAGE;
                const auto usage = resourceUsage(inputs.logical, id, is_image);
                result.resource_usage[r] = usage;
                if (is_image && rdesc::textureAspectMask(declaration.texture().format) == 6 &&
                    !inputs.device.separateDepthStencil())
                {
                    return cxx::unexpected(RenderError{kUnsupported, {id.value()}});
                }
                if (usage == 0)
                {
                    slot.resources.emplace_back(std::monostate{});
                }
                else if (declaration.origin == EGraphResourceOrigin::IMPORTED)
                {
                    const auto matches = [&](const auto& value) { return value.resource == id; };
                    if (std::count_if(inputs.imports.begin(), inputs.imports.end(), matches) != 1)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument, {id.value()}});
                    }
                    const auto& import = *std::find_if(inputs.imports.begin(), inputs.imports.end(), matches);
                    if (!import.identity.isValid())
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument, {id.value()}});
                    }
                    if (is_image)
                    {
                        auto* image = std::get_if<std::reference_wrapper<const Image>>(&import.backing);
                        if (!image || image->get().device() != result.device || (image->get().usage() & usage) != usage)
                        {
                            return cxx::unexpected(RenderError{kInvalidArgument, {id.value()}});
                        }
                        const auto& actual = image->get().description();
                        const auto& expected = declaration.texture();
                        const bool mismatch =
                            actual.extent.width != expected.width || actual.extent.height != expected.height ||
                            actual.format != nativeTextureFormat(expected.format) ||
                            actual.mip_levels != expected.mip_count || actual.array_layers != expected.array_layers ||
                            actual.samples != expected.samples;
                        if (mismatch)
                        {
                            return cxx::unexpected(RenderError{kInvalidArgument, {id.value()}});
                        }
                        slot.resources.emplace_back(*image);
                    }
                    else
                    {
                        auto* buffer = std::get_if<std::reference_wrapper<const Buffer>>(&import.backing);
                        const bool invalid = !buffer || buffer->get().device() != result.device ||
                                             buffer->get().size() < declaration.buffer().byte_size ||
                                             (buffer->get().usage() & usage) != usage;
                        if (invalid)
                        {
                            return cxx::unexpected(RenderError{kInvalidArgument, {id.value()}});
                        }
                        slot.resources.emplace_back(*buffer);
                    }
                }
                else if (is_image)
                {
                    const auto& texture = declaration.texture();
                    if (rdesc::textureAspectMask(texture.format) == 6 && !inputs.device.separateDepthStencil())
                    {
                        return cxx::unexpected(RenderError{kUnsupported, {id.value()}});
                    }
                    const bool unsupported = texture.dimension != ETextureDimension::D2 || texture.depth != 1 ||
                                             texture.extent_kind != EExtentKind::ABSOLUTE;
                    if (unsupported)
                    {
                        return cxx::unexpected(RenderError{kUnsupported, {id.value()}});
                    }
                    const bool aliasable = result.alias_sources[r].has_value() ||
                                           std::any_of(
                                               result.alias_sources.begin(),
                                               result.alias_sources.end(),
                                               [&](const auto& value) { return value && *value == id; }
                                           );
                    auto image = result.alias_sources[r]
                                     ? Image::alias(*imageOf(slot.resources[result.alias_sources[r]->value() - 1]))
                                     : Image::create(
                                           inputs.allocator,
                                           ImageDescription{
                                               {texture.width, texture.height},
                                               nativeTextureFormat(texture.format),
                                               usage,
                                               static_cast<VkSampleCountFlagBits>(texture.samples),
                                               texture.mip_count,
                                               texture.array_layers,
                                               aliasable
                                           }
                                       );
                    if (!image)
                    {
                        return cxx::unexpected(image.error());
                    }
                    const auto binding = image->memoryBinding();
                    slot.resources.emplace_back(std::move(*image));
                    if (binding.owns_allocation)
                    {
                        ++result.statistics.allocations;
                        result.statistics.allocated_bytes += binding.size;
                    }
                    else
                    {
                        ++result.statistics.aliased_resources;
                    }
                }
                else
                {
                    const bool readback = std::any_of(
                        graph.outputs.begin(),
                        graph.outputs.end(),
                        [&](const auto& output)
                        { return output.resource == id && output.kind == EGraphOutput::READBACK; }
                    );
                    auto buffer = Buffer::create(
                        inputs.allocator,
                        declaration.buffer().byte_size,
                        usage,
                        readback ? EMemoryAccess::READBACK : EMemoryAccess::DEVICE
                    );
                    if (!buffer)
                    {
                        return cxx::unexpected(buffer.error());
                    }
                    result.statistics.allocated_bytes += buffer->allocationBytes();
                    slot.resources.emplace_back(std::move(*buffer));
                    ++result.statistics.allocations;
                }
            }
        }
        return {};
    }
} // namespace lux::render::vulkan::detail
