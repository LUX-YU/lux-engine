#include "ImageFormat.hpp"
#include "LayoutPlan.hpp"
#include "Native.hpp"
#include "Shader.hpp"
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace lux::render::vulkan
{
    CompiledGraph::CompiledGraph(VkDevice device, TargetDesc target) noexcept : device_(device), target_(target) {}

    CompiledGraph::~CompiledGraph() noexcept
    {
        if (!device_)
        {
            return;
        }
        for (auto pipeline : pipelines_)
        {
            LUX_DESTROY("pipeline", vkDestroyPipeline(device_, pipeline, nullptr));
        }
        if (layout_)
        {
            LUX_DESTROY("pipeline_layout", vkDestroyPipelineLayout(device_, layout_, nullptr));
        }
        for (const auto layout : descriptor_layouts_)
        {
            LUX_DESTROY("shader_layout", vkDestroyDescriptorSetLayout(device_, layout, nullptr));
        }
    }

    CompiledGraph::CompiledGraph(CompiledGraph&& other) noexcept
        : descriptor_plan_(std::move(other.descriptor_plan_)), device_(std::exchange(other.device_, VK_NULL_HANDLE)),
          target_(other.target_), layout_(other.layout_), resources_(std::move(other.resources_)),
          storage_groups_(std::move(other.storage_groups_)), passes_(std::move(other.passes_)),
          host_barriers_(std::move(other.host_barriers_)), shader_identities_(std::move(other.shader_identities_)),
          pipelines_(std::move(other.pipelines_)), descriptor_layouts_(std::move(other.descriptor_layouts_)),
          output_(other.output_)
    {
    }

    bool CompiledGraph::compatible(const TargetDesc& target) const noexcept
    {
        return target.kind == target_.kind && target.format == target_.format && target.samples == target_.samples;
    }

    namespace
    {
        struct Access final
        {
            VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
            VkAccessFlags2 access = VK_ACCESS_2_NONE;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        };

        Access accessFor(const GraphResourceUse& use) noexcept
        {
            switch (use.usage)
            {
            case EGraphUse::COLOR_ATTACHMENT:
                return {
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                        (use.access == EGraphAccess::ATTACHMENT_LOAD ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : 0),
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                };
            case EGraphUse::DEPTH_ATTACHMENT:
                return {
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                };
            case EGraphUse::TRANSFER_SOURCE:
                return {
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                };
            case EGraphUse::VERTEX:
                return {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT};
            case EGraphUse::INDEX:
                return {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT};
            case EGraphUse::INDIRECT:
                return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
            case EGraphUse::STORAGE_WRITE:
                return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
            case EGraphUse::STORAGE_READ_WRITE:
                return {
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                };
            case EGraphUse::STORAGE_READ:
                return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
            case EGraphUse::SAMPLED:
                return {
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                };
            default:
                return {
                    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                };
            }
        }

        RenderResult<void> lowerDraw(
            const PassProgram& program,
            const GraphPass& pass,
            std::span<const NativeResourcePlan> resources,
            const VulkanDevice& device,
            CompiledPass& result
        ) noexcept
        {
            const auto find = [&](std::string_view key, EGraphUse role) -> RenderResult<std::uint32_t>
            {
                const auto found = std::ranges::find(resources, key, &NativeResourcePlan::key);
                if (found == resources.end() || !std::holds_alternative<GraphBufferDesc>(found->description))
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto index = std::uint32_t(found - resources.begin());
                const bool declared = std::ranges::any_of(
                    pass.uses,
                    [&](const auto& use) { return use.version.resource.index == index && use.usage == role; }
                );
                if (!declared)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                return index;
            };
            if (program.vertex_buffers.size() != program.graphics.vertex_bindings.size())
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            for (const auto& source : program.vertex_buffers)
            {
                const auto binding =
                    std::ranges::find(program.graphics.vertex_bindings, source.binding, &VertexBinding::binding);
                const bool duplicate = std::ranges::any_of(
                    result.vertices,
                    [&](const auto& other) { return other.binding == source.binding; }
                );
                if (binding == program.graphics.vertex_bindings.end() || duplicate || !binding->stride)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                auto resource = find(source.resource_key, EGraphUse::VERTEX);
                if (!resource)
                {
                    return cxx::unexpected(resource.error());
                }
                const auto size = std::get<GraphBufferDesc>(resources[*resource].description).size;
                if (source.offset >= size || source.offset % 4)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                result.vertices.push_back({source.binding, *resource, source.offset});
            }
            const auto index = [&](std::string_view key, std::uint64_t offset, EIndexType type) -> RenderResult<void>
            {
                if (type > EIndexType::UINT32)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                auto resource = find(key, EGraphUse::INDEX);
                if (!resource)
                {
                    return cxx::unexpected(resource.error());
                }
                const auto bytes = type == EIndexType::UINT16 ? 2u : 4u;
                const auto size = std::get<GraphBufferDesc>(resources[*resource].description).size;
                if (offset >= size || offset % bytes || size - offset < bytes)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                result.index_resource = *resource;
                result.index_offset = offset;
                result.index_type = type == EIndexType::UINT16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
                return {};
            };
            if (const auto* draw = std::get_if<DrawVertices>(&program.draw))
            {
                result.count = draw->count;
                result.instances = draw->instances;
                result.first = draw->first;
                result.first_instance = draw->first_instance;
            }
            else if (const auto* draw = std::get_if<DrawIndexed>(&program.draw))
            {
                auto ready = index(draw->index_resource, draw->offset, draw->type);
                if (!ready)
                {
                    return ready;
                }
                const auto size = std::get<GraphBufferDesc>(resources[result.index_resource].description).size;
                const auto element = draw->type == EIndexType::UINT16 ? 2u : 4u;
                if (std::uint64_t(draw->first) + draw->count > (size - draw->offset) / element)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                result.draw = EDrawRecipe::INDEXED;
                result.count = draw->count;
                result.instances = draw->instances;
                result.first = draw->first;
                result.first_instance = draw->first_instance;
                result.vertex_offset = draw->vertex_offset;
            }
            else
            {
                const auto& indirect = std::get<DrawIndexedIndirect>(program.draw);
                auto ready = index(indirect.index_resource, indirect.index_offset, indirect.type);
                if (!ready)
                {
                    return ready;
                }
                auto resource = find(indirect.commands_resource, EGraphUse::INDIRECT);
                if (!resource)
                {
                    return cxx::unexpected(resource.error());
                }
                const auto size = std::get<GraphBufferDesc>(resources[*resource].description).size;
                const bool invalid = !indirect.count || indirect.commands_offset % 4 || indirect.stride % 4 ||
                                     indirect.stride < sizeof(VkDrawIndexedIndirectCommand) ||
                                     indirect.commands_offset > size;
                if (invalid)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const auto remaining = size - indirect.commands_offset;
                if (remaining < sizeof(VkDrawIndexedIndirectCommand) ||
                    indirect.count - 1 > (remaining - sizeof(VkDrawIndexedIndirectCommand)) / indirect.stride)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                // This Device currently enables single indirect indirect. Multiple commands require the
                // actual multiDrawIndirect feature, not merely a larger author-provided count.
                if (indirect.count > 1 || indirect.count > device.properties().limits.maxDrawIndirectCount)
                {
                    return cxx::unexpected(RenderError{kUnsupported});
                }
                result.draw = EDrawRecipe::INDEXED_INDIRECT;
                result.indirect_resource = *resource;
                result.indirect_offset = indirect.commands_offset;
                result.indirect_stride = indirect.stride;
                result.count = indirect.count;
            }
            if (!result.count || !result.instances)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            // Exact direct-vertex bounds; indexed values are trusted GPU/validated asset data, not read back here.
            for (const auto& source : result.vertices)
            {
                const auto& layout =
                    *std::ranges::find(program.graphics.vertex_bindings, source.binding, &VertexBinding::binding);
                const auto full_size = std::get<GraphBufferDesc>(resources[source.resource].description).size;
                const auto size = full_size - source.offset;
                for (const auto& attribute : program.graphics.vertex_attributes)
                {
                    if (attribute.binding != source.binding)
                    {
                        continue;
                    }
                    const auto bytes = vertexFormat(attribute.format).bytes();
                    if (!bytes || attribute.offset > size || bytes > size - attribute.offset)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                    std::uint64_t last = 0;
                    if (result.draw == EDrawRecipe::VERTICES && layout.rate == EVertexRate::VERTEX)
                    {
                        last = std::uint64_t(result.first) + result.count - 1;
                    }
                    else if (result.draw != EDrawRecipe::INDEXED_INDIRECT && layout.rate == EVertexRate::INSTANCE)
                    {
                        last = std::uint64_t(result.first_instance) + result.instances - 1;
                    }
                    if (last > (size - attribute.offset - bytes) / layout.stride)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                }
            }
            return {};
        }

        bool fullRange(const GraphResourceUse& use, const NativeResourcePlan& resource) noexcept
        {
            if (auto* b = std::get_if<GraphBufferDesc>(&resource.description))
            {
                return std::get<GraphBufferRange>(use.range) == GraphBufferRange{0, b->size};
            }
            const auto& image = std::get<GraphImageDesc>(resource.description);
            return std::get<GraphImageRange>(use.range) ==
                   GraphImageRange{rdesc::textureAspectMask(image.format), 0, image.mips, 0, image.layers};
        }
    } // namespace

    RenderResult<CompiledGraph> compileGraph(
        const DeviceSession& session,
        const RenderGraphDefinition& graph,
        const GraphAnalysis& analysis,
        const GraphStorageBindings& external_bindings,
        std::span<const PassProgram> programs,
        const TargetDesc& target
    ) noexcept
    {
        if (auto result = target.validate(); !result)
        {
            return cxx::unexpected(result.error());
        }
        const auto& device = session.device();
        const bool is_unsupported_target = target.kind != ETargetKind::OFFSCREEN || target.samples != 1 ||
                                           imageFormat(target.format).native == VK_FORMAT_UNDEFINED ||
                                           target.width > device.properties().limits.maxImageDimension2D ||
                                           target.height > device.properties().limits.maxImageDimension2D;
        if (is_unsupported_target)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        CompiledGraph result(device.native(), target);
        // Resolve author keys once at the cold compiler boundary, then index by GraphPassId.
        std::vector<const PassProgram*> pass_programs(graph.passes().size(), nullptr);
        for (const auto& program : programs)
        {
            const auto found = std::ranges::find(graph.passes(), program.pass_key, &GraphPass::key);
            if (found == graph.passes().end())
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto index = static_cast<std::size_t>(found - graph.passes().begin());
            if (pass_programs[index])
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            pass_programs[index] = &program;
        }
        std::vector<bool> live_resources(graph.resources().size(), false);
        for (const auto pass : analysis.execution_order)
        {
            for (const auto& use : graph.passes()[pass.index].uses)
            {
                live_resources[use.version.resource.index] = true;
            }
        }
        for (const auto& output : graph.outputs())
        {
            live_resources[output.version.resource.index] = true;
        }
        for (const auto& resource : graph.resources())
        {
            const auto index = result.resources_.size();
            result.resources_.push_back({resource.key, resource.description, resource.origin});
            if (!live_resources[index])
            {
                continue;
            }
            const bool is_history =
                resource.origin == EGraphOrigin::HISTORY_PREVIOUS || resource.origin == EGraphOrigin::HISTORY_CURRENT;
            if (is_history)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            if (const auto* image = std::get_if<GraphImageDesc>(&resource.description))
            {
                const bool is_imported = resource.origin == EGraphOrigin::IMPORTED;
                const bool sampled_format = image->format == rdesc::ETextureFormat::RGBA8_UNORM ||
                                            image->format == rdesc::ETextureFormat::RGBA8_SRGB ||
                                            image->format == rdesc::ETextureFormat::BC1_RGBA_SRGB;
                const bool is_unsupported_image =
                    image->layers != 1 || image->samples != 1 ||
                    (is_imported
                         ? !sampled_format
                         : image->mips != 1 || imageFormat(image->format).native == VK_FORMAT_UNDEFINED ||
                               imageFormat(image->format).block != 1 || resource.origin != EGraphOrigin::TRANSIENT);
                if (is_unsupported_image)
                {
                    return cxx::unexpected(RenderError{kUnsupported});
                }
            }
        }
        const auto install = [&](std::span<const GraphStorageBinding> source,
                                 std::vector<std::uint32_t>& indices) -> RenderResult<void>
        {
            for (const auto& binding : source)
            {
                if (binding.resource.index >= result.resources_.size())
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                if (!live_resources[binding.resource.index])
                {
                    continue;
                }
                auto& resource = result.resources_[binding.resource.index];
                if (resource.storage)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const auto* factory = session.resources().factory(binding.storage.type.view());
                if (!factory || !factory->binding_ops ||
                    !static_cast<const StorageBindingOps*>(factory->binding_ops)->prepare)
                {
                    return cxx::unexpected(RenderError{kStorageFactory});
                }
                resource.storage = binding.storage;
                indices.push_back(binding.resource.index);
            }
            return {};
        };
        if (auto r = install(external_bindings.global, result.storage_groups_.global); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = install(external_bindings.scene, result.storage_groups_.scene); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = install(external_bindings.view, result.storage_groups_.view); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = install(external_bindings.attachment, result.storage_groups_.attachment); !r)
        {
            return cxx::unexpected(r.error());
        }
        for (const auto& output : graph.outputs())
        {
            auto& plan = result.resources_[output.version.resource.index];
            if (std::holds_alternative<GraphBufferDesc>(plan.description))
            {
                plan.readback = true;
            }
            if (!output.target)
            {
                continue;
            }
            if (plan.origin != EGraphOrigin::TRANSIENT)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            if (*output.target != target || result.output_ != kGraphInvalidIndex)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            result.output_ = output.version.resource.index;
            plan.target_sized = true;
        }
        if (result.output_ == kGraphInvalidIndex)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        // All attachments in one rendering scope have the same resize contract. Propagate only this
        // physical extent constraint across shared Depth attachments; execution order/analysis is untouched.
        bool changed;
        do
        {
            changed = false;
            for (const auto id : analysis.execution_order)
            {
                const auto& pass = graph.passes()[id.index];
                if (pass.kind != EGraphPassKind::GRAPHICS)
                {
                    continue;
                }
                NativeResourcePlan* color = nullptr;
                NativeResourcePlan* depth = nullptr;
                for (const auto& use : pass.uses)
                {
                    if (use.usage == EGraphUse::COLOR_ATTACHMENT)
                    {
                        color = &result.resources_[use.version.resource.index];
                    }
                    if (use.usage == EGraphUse::DEPTH_ATTACHMENT)
                    {
                        depth = &result.resources_[use.version.resource.index];
                    }
                }
                if (!color || !depth)
                {
                    continue;
                }
                const auto& c = std::get<GraphImageDesc>(color->description);
                const auto& d = std::get<GraphImageDesc>(depth->description);
                if (c.width != d.width || c.height != d.height)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                if (color->target_sized != depth->target_sized)
                {
                    color->target_sized = depth->target_sized = true;
                    changed = true;
                }
            }
        } while (changed);
        auto planned = planLayouts(device, graph, analysis, pass_programs);
        if (!planned)
        {
            return cxx::unexpected(planned.error());
        }
        result.descriptor_plan_ = std::make_unique<LayoutPlan>(std::move(*planned));
        const auto& layout_plan = *result.descriptor_plan_;
        if (!layout_plan.shared_bindings.empty())
        {
            auto capacity = session.sampledImageCapacity();
            if (!capacity)
            {
                return cxx::unexpected(capacity.error());
            }
            auto layout = createDescriptorLayout(device, layout_plan.shared_bindings, *capacity, "shader_layout");
            if (!layout)
            {
                return cxx::unexpected(layout.error());
            }
            result.descriptor_layouts_.push_back(*layout);
        }
        if (!layout_plan.private_bindings.empty())
        {
            auto layout = createDescriptorLayout(device, layout_plan.private_bindings, 0, "shader_layout");
            if (!layout)
            {
                return cxx::unexpected(layout.error());
            }
            result.descriptor_layouts_.push_back(*layout);
        }
        std::vector<Access> previous(result.resources_.size());
        std::vector<bool> used(result.resources_.size(), false);
        // INVARIANT: exact Graph analysis determines order; no graph reconstruction, pruning or re-analysis.
        for (auto id : analysis.execution_order)
        {
            const auto& pass = graph.passes()[id.index];
            const auto* binding = pass_programs[id.index];
            if (!binding)
            {
                return cxx::unexpected(RenderError{kInvalidArgument, {id.index}});
            }
            const bool is_unsupported_pass = pass.conditional || pass.uses.empty();
            if (is_unsupported_pass)
            {
                return cxx::unexpected(RenderError{kUnsupported, {id.index}});
            }
            if (!std::ranges::all_of(binding->clear_color, [](float x) { return std::isfinite(x); }))
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            CompiledPass compiled{};
            compiled.layout_program = id.index;
            compiled.clear_color = binding->clear_color;
            if (!std::isfinite(binding->clear_depth) || binding->clear_depth < 0 || binding->clear_depth > 1)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            compiled.clear_depth = binding->clear_depth;
            compiled.dispatch = binding->dispatch;
            const bool compute = pass.kind == EGraphPassKind::COMPUTE;
            if (compute)
            {
                if (!(device.queue(EQueueRole::GRAPHICS).flags & VK_QUEUE_COMPUTE_BIT))
                {
                    return cxx::unexpected(RenderError{kUnsupported});
                }
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    if (!binding->dispatch[axis] ||
                        binding->dispatch[axis] > device.properties().limits.maxComputeWorkGroupCount[axis])
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                }
            }
            unsigned reads = 0, writes = 0;
            for (const auto& use : pass.uses)
            {
                const auto index = use.version.resource.index;
                auto& resource = result.resources_[index];
                const bool is_unsupported_use =
                    use.usage == EGraphUse::UNSPECIFIED || use.fallback || !fullRange(use, resource);
                if (is_unsupported_use)
                {
                    return cxx::unexpected(RenderError{kUnsupported, {id.index, index}});
                }
                if (use.usage == EGraphUse::COLOR_ATTACHMENT || use.usage == EGraphUse::DEPTH_ATTACHMENT)
                {
                    auto& attachment = use.usage == EGraphUse::COLOR_ATTACHMENT ? compiled.color : compiled.depth;
                    if (attachment != kGraphInvalidIndex)
                    {
                        return cxx::unexpected(RenderError{kUnsupported});
                    }
                    attachment = index;
                }
                if (use.access == EGraphAccess::READ)
                {
                    compiled.source = index;
                    ++reads;
                }
                else
                {
                    compiled.destination = index;
                    ++writes;
                }
                if (resource.origin == EGraphOrigin::IMPORTED && use.access != EGraphAccess::READ)
                {
                    return cxx::unexpected(RenderError{kUnsupported});
                }
                const bool storage_use = use.usage == EGraphUse::STORAGE_READ ||
                                         use.usage == EGraphUse::STORAGE_WRITE ||
                                         use.usage == EGraphUse::STORAGE_READ_WRITE;
                if (use.usage == EGraphUse::SAMPLED || storage_use)
                {
                    const bool is_image = std::holds_alternative<GraphImageDesc>(resource.description);
                    const bool is_invalid_binding = (resource.origin == EGraphOrigin::IMPORTED && !resource.storage) ||
                                                    (is_image && resource.origin != EGraphOrigin::IMPORTED) ||
                                                    is_image != (use.usage == EGraphUse::SAMPLED);
                    if (is_invalid_binding)
                    {
                        return cxx::unexpected(RenderError{kStorageContract});
                    }
                    if (is_image)
                    {
                        resource.image_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
                        // Published storage images already have Graphics ownership and SHADER_READ layout.
                        used[index] = true;
                        previous[index] = accessFor(use);
                        continue;
                    }
                }
                if (resource.origin == EGraphOrigin::IMPORTED &&
                    std::holds_alternative<GraphImageDesc>(resource.description))
                {
                    return cxx::unexpected(RenderError{kUnsupported});
                }
                auto current = accessFor(use);
                if (storage_use)
                {
                    current.stage = VK_PIPELINE_STAGE_2_NONE;
                    const auto& program = layout_plan.programs[id.index];
                    for (std::size_t field = 0; field < program.uses.size(); ++field)
                    {
                        if (program.uses[field].resource != index)
                        {
                            continue;
                        }
                        const auto stages = program.bindings[field].stages;
                        if (stages & 1)
                        {
                            current.stage |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
                        }
                        if (stages & 2)
                        {
                            current.stage |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                        }
                        if (stages & 4)
                        {
                            current.stage |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        }
                    }
                }
                const auto source = previous[index];
                if (std::holds_alternative<GraphImageDesc>(resource.description))
                {
                    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                    barrier.srcStageMask = source.stage;
                    barrier.srcAccessMask = source.access;
                    barrier.dstStageMask = current.stage;
                    barrier.dstAccessMask = current.access;
                    barrier.oldLayout = source.layout;
                    barrier.newLayout = current.layout;
                    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.subresourceRange =
                        {imageFormat(std::get<GraphImageDesc>(resource.description).format).aspects, 0, 1, 0, 1};
                    compiled.images.push_back({index, barrier});
                    resource.image_usage |=
                        use.usage == EGraphUse::COLOR_ATTACHMENT   ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                        : use.usage == EGraphUse::DEPTH_ATTACHMENT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        : use.usage == EGraphUse::TRANSFER_SOURCE  ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                                                   : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                    if (use.access == EGraphAccess::ATTACHMENT_LOAD)
                    {
                        auto& load = use.usage == EGraphUse::DEPTH_ATTACHMENT ? compiled.depth_load : compiled.load;
                        load = VK_ATTACHMENT_LOAD_OP_LOAD;
                    }
                }
                else
                {
                    VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                    barrier.srcStageMask = source.stage;
                    barrier.srcAccessMask = source.access;
                    if (!used[index])
                    {
                        // The same physical transient Buffer is reused on the next Execute. Order prior
                        // device accesses as well as imported host/transfer publication before its first use.
                        barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                        barrier.srcAccessMask =
                            VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
                    }
                    barrier.dstStageMask = current.stage;
                    barrier.dstAccessMask = current.access;
                    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.size = VK_WHOLE_SIZE;
                    compiled.buffers.push_back({index, barrier});
                    resource.buffer_usage |= storage_use                        ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                             : use.usage == EGraphUse::VERTEX   ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                             : use.usage == EGraphUse::INDEX    ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                             : use.usage == EGraphUse::INDIRECT ? VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                             : use.usage == EGraphUse::TRANSFER_SOURCE
                                                 ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                                 : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                }
                used[index] = true;
                previous[index] = current;
            }
            const bool graphics = pass.kind == EGraphPassKind::GRAPHICS;
            const auto attachments =
                unsigned(compiled.color != kGraphInvalidIndex) + unsigned(compiled.depth != kGraphInvalidIndex);
            if (graphics)
            {
                compiled.destination = compiled.color != kGraphInvalidIndex ? compiled.color : compiled.depth;
            }
            const bool is_unsupported_recipe =
                (graphics && (!attachments || writes != attachments)) ||
                (pass.kind == EGraphPassKind::TRANSFER && writes != 1) || (compute && writes == 0) ||
                (pass.kind == EGraphPassKind::TRANSFER && reads > 1) ||
                (pass.kind == EGraphPassKind::TRANSFER && !binding->shader.stages.empty());
            if (is_unsupported_recipe)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            const bool writes_image =
                std::holds_alternative<GraphImageDesc>(result.resources_[compiled.destination].description);
            if (pass.kind == EGraphPassKind::GRAPHICS || compute)
            {
                if (!compute && !writes_image)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                compiled.recipe = compute ? ERecipe::DISPATCH : ERecipe::DRAW;
                if (!compute)
                {
                    auto lowered = lowerDraw(*binding, pass, result.resources_, device, compiled);
                    if (!lowered)
                    {
                        return cxx::unexpected(lowered.error());
                    }
                }
                if (!result.layout_)
                {
                    const VkPushConstantRange range{layout_plan.parameter_stages, 0, layout_plan.parameter_bytes};
                    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                    info.pushConstantRangeCount = unsigned(layout_plan.parameter_bytes != 0);
                    info.pPushConstantRanges = &range;
                    info.setLayoutCount = std::uint32_t(result.descriptor_layouts_.size());
                    info.pSetLayouts = result.descriptor_layouts_.data();
                    const auto created = LUX_NATIVE(
                        "pipeline_layout",
                        vkCreatePipelineLayout(device.native(), &info, nullptr, &result.layout_)
                    );
                    if (created != VK_SUCCESS)
                    {
                        return cxx::unexpected(nativeError(created));
                    }
                }
                // The Graph owns one compatible physical descriptor/parameter layout. Compare complete
                // stage bytes + ABI; draw count and clear color are not Pipeline identity.
                auto shader = relocateShader(binding->shader, layout_plan.programs[id.index].bindings);
                if (!shader)
                {
                    return cxx::unexpected(shader.error());
                }
                CompiledGraph::PipelineIdentity identity{
                    std::move(*shader),
                    compute ? GraphicsState{} : binding->graphics
                };
                if (compiled.color != kGraphInvalidIndex)
                {
                    identity.color =
                        imageFormat(std::get<GraphImageDesc>(result.resources_[compiled.color].description).format)
                            .native;
                }
                if (compiled.depth != kGraphInvalidIndex)
                {
                    identity.depth =
                        imageFormat(std::get<GraphImageDesc>(result.resources_[compiled.depth].description).format)
                            .native;
                }
                std::ranges::sort(identity.graphics.vertex_bindings, {}, &VertexBinding::binding);
                std::ranges::sort(identity.graphics.vertex_attributes, {}, &VertexAttribute::location);
                const auto same = std::ranges::find(result.shader_identities_, identity);
                if (same != result.shader_identities_.end())
                {
                    compiled.pipeline = result.pipelines_[same - result.shader_identities_.begin()];
                }
                else
                {
                    auto pipeline = compute ? compileCompute(device, result.layout_, identity.shader)
                                            : compileGraphics(
                                                  device,
                                                  result.layout_,
                                                  identity.shader,
                                                  identity.graphics,
                                                  identity.color,
                                                  identity.depth
                                              );
                    if (!pipeline)
                    {
                        return cxx::unexpected(pipeline.error());
                    }
                    compiled.pipeline = *pipeline;
                    result.pipelines_.push_back(*pipeline);
                    result.shader_identities_.push_back(std::move(identity));
                }
            }
            else if (compiled.source == kGraphInvalidIndex)
            {
                if (!writes_image &&
                    std::get<GraphBufferDesc>(result.resources_[compiled.destination].description).size % 4 != 0)
                {
                    return cxx::unexpected(RenderError{kUnsupported});
                }
                compiled.recipe = writes_image ? ERecipe::CLEAR_IMAGE : ERecipe::FILL_BUFFER;
            }
            else
            {
                if (writes_image)
                {
                    return cxx::unexpected(RenderError{kUnsupported});
                }
                const auto& source = result.resources_[compiled.source];
                auto& destination = result.resources_[compiled.destination];
                const auto size = std::get<GraphBufferDesc>(destination.description).size;
                if (const auto* image = std::get_if<GraphImageDesc>(&source.description))
                {
                    if (size != std::uint64_t(image->width) * image->height * imageFormat(image->format).bytes)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                    compiled.recipe = ERecipe::COPY_IMAGE_BUFFER;
                    destination.target_sized = source.target_sized;
                    destination.target_pixel_bytes = imageFormat(image->format).bytes;
                }
                else
                {
                    if (size != std::get<GraphBufferDesc>(source.description).size)
                    {
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    }
                    compiled.recipe = ERecipe::COPY_BUFFER;
                }
            }
            result.passes_.push_back(std::move(compiled));
        }
        // Lower the fixed legal execution order to static record decisions. No Pass reordering.
        VkPipeline bound = VK_NULL_HANDLE;
        for (std::size_t i = 0; i < result.passes_.size(); ++i)
        {
            auto& current = result.passes_[i];
            if (current.recipe != ERecipe::DRAW)
            {
                continue;
            }
            current.bind_pipeline = bound != current.pipeline;
            bound = current.pipeline; // transfer commands and Begin/EndRendering do not invalidate this state
            if (i == 0)
            {
                continue;
            }
            auto& previous = result.passes_[i - 1];
            const bool same_attachment = previous.recipe == ERecipe::DRAW && previous.color == current.color &&
                                         previous.depth == kGraphInvalidIndex && current.depth == kGraphInvalidIndex;
            const bool preserves_content = current.load == VK_ATTACHMENT_LOAD_OP_LOAD;
            const bool attachment_only = current.images.size() == 1 && current.buffers.empty() &&
                                         current.images[0].native.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            // Sampled inputs are immutable, disjoint Global images, with no read/read barriers.
            // Rasterization-order color attachment access preserves LOAD within one scope.
            const bool can_merge = same_attachment && preserves_content && attachment_only;
            if (!can_merge)
            {
                continue;
            }
            previous.end_rendering = false;
            current.begin_rendering = false;
            current.images.clear();
        }
        for (std::uint32_t i = 0; i < result.resources_.size(); ++i)
        {
            const auto& resource = result.resources_[i];
            if (used[i] && resource.origin == EGraphOrigin::IMPORTED && !resource.storage)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            if (used[i] && resource.image_usage)
            {
                const auto format = imageFormat(std::get<GraphImageDesc>(resource.description).format).native;
                VkFormatProperties properties{};
                vkGetPhysicalDeviceFormatProperties(device.physical(), format, &properties);
                VkFormatFeatureFlags required = 0;
                if (resource.image_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                {
                    required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
                }
                if (resource.image_usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
                {
                    required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
                }
                if (resource.image_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                {
                    required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
                }
                if (resource.image_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                {
                    required |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
                }
                if ((properties.optimalTilingFeatures & required) != required)
                {
                    return cxx::unexpected(RenderError{kUnsupported});
                }
            }
            if (!used[i] || !resource.readback)
            {
                continue;
            }
            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcStageMask = previous[i].stage;
            barrier.srcAccessMask = previous[i].access;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.size = VK_WHOLE_SIZE;
            result.host_barriers_.push_back({i, barrier});
        }
        return result;
    }
} // namespace lux::render::vulkan
