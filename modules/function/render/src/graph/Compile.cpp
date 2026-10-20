#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/CompiledGraph.hpp>
#include <lux/engine/render/graph/LayoutPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace lux::render::detail
{
    CompiledGraph::CompiledGraph(nri::Device& device, const nri::CoreInterface& api, TargetDesc target) noexcept
        : device_(&device), nri_core_(&api), target_(target)
    {
    }

    CompiledGraph::~CompiledGraph() noexcept
    {
        if (!device_)
        {
            return;
        }
        for (auto pipeline : pipelines_)
        {
            nri_core_->DestroyPipeline(pipeline);
        }
        if (layout_)
        {
            nri_core_->DestroyPipelineLayout(layout_);
        }
    }

    CompiledGraph::CompiledGraph(CompiledGraph&& other) noexcept
        : descriptor_plan_(std::move(other.descriptor_plan_)), device_(std::exchange(other.device_, nullptr)),
          nri_core_(other.nri_core_), target_(other.target_), layout_(other.layout_),
          resources_(std::move(other.resources_)), frame_contributions_(std::move(other.frame_contributions_)),
          storage_groups_(std::move(other.storage_groups_)), passes_(std::move(other.passes_)),
          pass_indices_(std::move(other.pass_indices_)), pipelines_(std::move(other.pipelines_)),
          output_(other.output_), resizable_output_(other.resizable_output_)
    {
    }

    bool CompiledGraph::compatible(const TargetDesc& target) const noexcept
    {
        const bool same_type =
            target.kind == target_.kind && target.format == target_.format && target.samples == target_.samples;
        if (!same_type)
        {
            return false;
        }
        const bool resized = target.width != target_.width || target.height != target_.height;
        if (resized && !resizable_output_)
        {
            return false;
        }
        if (resized)
        {
            for (const auto& pass : passes_)
            {
                // An explicit image-to-buffer Copy retains its declared byte extent. There is no
                // implicit readback allocation to resize behind the author's resource description.
                if (pass.recipe == ERecipe::COPY_IMAGE_BUFFER && resources_[pass.source].target_sized)
                {
                    return false;
                }
            }
        }
        return true;
    }

    namespace
    {
        struct Access final
        {
            nri::StageBits stage = nri::StageBits::NONE;
            nri::AccessBits access = nri::AccessBits::NONE;
            nri::Layout layout = nri::Layout::UNDEFINED;
        };

        Access accessFor(const GraphResourceUse& use) noexcept
        {
            switch (use.usage)
            {
            case EGraphUse::COLOR_ATTACHMENT:
                return {
                    nri::StageBits::COLOR_ATTACHMENT,
                    nri::AccessBits::COLOR_ATTACHMENT_WRITE |
                        (use.access == EGraphAccess::ATTACHMENT_LOAD ? nri::AccessBits::COLOR_ATTACHMENT_READ
                                                                     : nri::AccessBits::NONE),
                    nri::Layout::COLOR_ATTACHMENT
                };
            case EGraphUse::DEPTH_ATTACHMENT:
                return {
                    nri::StageBits::DEPTH_STENCIL_ATTACHMENT,
                    nri::AccessBits::DEPTH_STENCIL_ATTACHMENT,
                    nri::Layout::DEPTH_STENCIL_ATTACHMENT
                };
            case EGraphUse::TRANSFER_SOURCE:
                return {nri::StageBits::COPY, nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE};
            case EGraphUse::VERTEX:
                return {nri::StageBits::VERTEX_SHADER, nri::AccessBits::VERTEX_BUFFER};
            case EGraphUse::INDEX:
                return {nri::StageBits::INDEX_INPUT, nri::AccessBits::INDEX_BUFFER};
            case EGraphUse::INDIRECT:
                return {nri::StageBits::INDIRECT, nri::AccessBits::ARGUMENT_BUFFER};
            case EGraphUse::STORAGE_WRITE:
                return {nri::StageBits::COMPUTE_SHADER, nri::AccessBits::SHADER_RESOURCE_STORAGE};
            case EGraphUse::STORAGE_READ_WRITE:
                return {nri::StageBits::COMPUTE_SHADER, nri::AccessBits::SHADER_RESOURCE_STORAGE};
            case EGraphUse::STORAGE_READ:
                return {nri::StageBits::FRAGMENT_SHADER, nri::AccessBits::SHADER_RESOURCE_STORAGE};
            case EGraphUse::SAMPLED:
                return {
                    nri::StageBits::FRAGMENT_SHADER,
                    nri::AccessBits::SHADER_RESOURCE,
                    nri::Layout::SHADER_RESOURCE
                };
            default:
                return {nri::StageBits::COPY, nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION};
            }
        }

        RenderResult<void> lowerDraw(
            const PassProgram& program,
            const GraphPass& pass,
            std::span<const CompiledResource> resources,
            const ResourceNames& resource_names,
            const nri::DeviceDesc& limits,
            CompiledPass& result
        ) noexcept
        {
            const auto find = [&](std::string_view key, EGraphUse role) -> RenderResult<std::uint32_t>
            {
                const auto found = resource_names.find(key);
                if (found == resource_names.end() ||
                    !std::holds_alternative<GraphBufferDesc>(resources[found->second].description))
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto index = found->second;
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
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                result.vertices.push_back({source.binding, *resource, binding->stride, source.offset});
            }
            const auto index = [&](std::string_view key, std::uint64_t offset, EIndexType type) -> RenderResult<void>
            {
                if (type > EIndexType::UINT32)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
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
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                result.index_resource = *resource;
                result.index_offset = offset;
                result.index_type = type == EIndexType::UINT16 ? nri::IndexType::UINT16 : nri::IndexType::UINT32;
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
                    return cxx::unexpected(RenderError{kDeviceArgument});
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
                                     indirect.stride < sizeof(nri::DrawIndexedDesc) || indirect.commands_offset > size;
                if (invalid)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                const auto remaining = size - indirect.commands_offset;
                if (remaining < sizeof(nri::DrawIndexedDesc) ||
                    indirect.count - 1 > (remaining - sizeof(nri::DrawIndexedDesc)) / indirect.stride)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                // This Device currently enables single indirect indirect. Multiple commands require the
                // actual multiDrawIndirect feature, not merely a larger author-provided count.
                if (indirect.count > 1 || indirect.count > limits.other.drawIndirectMaxNum)
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                result.draw = EDrawRecipe::INDEXED_INDIRECT;
                result.indirect_resource = *resource;
                result.indirect_offset = indirect.commands_offset;
                result.indirect_stride = indirect.stride;
                result.count = indirect.count;
            }
            if (!result.count || !result.instances)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
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
                        return cxx::unexpected(RenderError{kDeviceArgument});
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
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                }
            }
            return {};
        }

        bool fullRange(const GraphResourceUse& use, const CompiledResource& resource) noexcept
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

    RenderResult<std::vector<const PassProgram*>> indexPrograms(
        const RenderGraphDefinition& graph,
        std::span<const PassProgram> programs
    ) noexcept
    {
        std::map<std::string_view, std::size_t, std::less<>> names;
        for (std::size_t i = 0; i < graph.passes().size(); ++i)
        {
            names.emplace(graph.passes()[i].key, i);
        }
        std::vector<const PassProgram*> result(graph.passes().size(), nullptr);
        for (std::size_t i = 0; i < programs.size(); ++i)
        {
            const auto found = names.find(programs[i].pass_key);
            if (found == names.end())
            {
                return cxx::unexpected(RenderError{kDeviceArgument, {i, 0}});
            }
            if (result[found->second])
            {
                return cxx::unexpected(RenderError{kDeviceArgument, {i, 1}});
            }
            result[found->second] = &programs[i];
        }
        return result;
    }

    RenderResult<CompiledGraph> compileGraph(
        nri::Device& device,
        const nri::CoreInterface& api,
        const EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::QueueType queue_type,
        std::uint32_t sampled_image_capacity,
        std::uint32_t sampler_capacity,
        const RenderGraphDefinition& graph,
        const GraphAnalysis& analysis,
        GraphStorageBindings external_bindings,
        std::span<const PassProgram* const> pass_programs,
        const TargetDesc& target,
        std::vector<FeatureFrameContribution> contexts
    ) noexcept
    {
        if (auto result = target.validate(); !result)
        {
            return cxx::unexpected(result.error());
        }
        const auto& limits = api.GetDeviceDesc(device);
        const bool is_unsupported_target = target.kind != ETargetKind::OFFSCREEN || target.samples != 1 ||
                                           imageFormat(target.format).native == nri::Format::UNKNOWN ||
                                           target.width > limits.dimensions.texture2DMaxDim ||
                                           target.height > limits.dimensions.texture2DMaxDim;
        if (is_unsupported_target)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }

        struct PipelineIdentity final
        {
            rdesc::SpirvArtifact shader;
            GraphicsState graphics;
            nri::Format color = nri::Format::UNKNOWN, depth = nri::Format::UNKNOWN;
            bool operator==(const PipelineIdentity&) const noexcept = default;
        };

        std::vector<PipelineIdentity> pipeline_identities;
        ResourceNames resource_names;
        for (std::uint32_t index = 0; index < graph.resources().size(); ++index)
        {
            resource_names.emplace(graph.resources()[index].key, index);
        }
        CompiledGraph result(device, api, target);
        if (pass_programs.size() != graph.passes().size())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
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
            result.resources_.back().used = live_resources[index];
            if (!live_resources[index])
            {
                continue;
            }
            const bool is_history =
                resource.origin == EGraphOrigin::HISTORY_PREVIOUS || resource.origin == EGraphOrigin::HISTORY_CURRENT;
            if (is_history)
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported});
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
                         : image->mips != 1 || imageFormat(image->format).native == nri::Format::UNKNOWN ||
                               imageFormat(image->format).block != 1 || resource.origin != EGraphOrigin::TRANSIENT);
                if (is_unsupported_image)
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
            }
        }
        const auto install = [&](std::span<GraphStorageBinding> source,
                                 std::vector<std::uint32_t>& indices) -> RenderResult<void>
        {
            for (auto& binding : source)
            {
                if (binding.resource.index >= result.resources_.size())
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                if (!live_resources[binding.resource.index])
                {
                    continue;
                }
                auto& resource = result.resources_[binding.resource.index];
                if (resource.storage)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                resource.storage = std::move(binding.storage);
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
        for (auto& contribution : contexts)
        {
            const auto index = contribution.resource.index;
            if (index >= result.resources_.size() || !contribution.create || !contribution.configuration)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            if (!live_resources[index])
            {
                continue;
            }
            auto& resource = result.resources_[index];
            const bool is_invalid = resource.storage || resource.context_data != kGraphInvalidIndex ||
                                    resource.origin != EGraphOrigin::IMPORTED ||
                                    !std::holds_alternative<GraphBufferDesc>(resource.description);
            if (is_invalid)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            resource.context_data = static_cast<std::uint32_t>(result.frame_contributions_.size());
            result.frame_contributions_.push_back(std::move(contribution));
        }
        for (const auto& output : graph.outputs())
        {
            auto& plan = result.resources_[output.version.resource.index];
            if (!output.target)
            {
                continue;
            }
            if (plan.origin != EGraphOrigin::TRANSIENT)
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            }
            const auto& required = *output.target;
            const auto& image = std::get<GraphImageDesc>(plan.description);
            const bool relative = required.width == 0 && required.height == 0;
            const bool incompatible =
                required.format != target.format || required.samples != target.samples || image.width != target.width ||
                image.height != target.height ||
                (!relative && (required.width != target.width || required.height != target.height));
            if (incompatible || result.output_ != kGraphInvalidIndex)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            result.output_ = output.version.resource.index;
            result.resizable_output_ = relative;
            plan.target_sized = relative;
        }
        if (result.output_ == kGraphInvalidIndex)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
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
                CompiledResource* color = nullptr;
                CompiledResource* depth = nullptr;
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
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                if (color->target_sized != depth->target_sized)
                {
                    color->target_sized = depth->target_sized = true;
                    changed = true;
                }
            }
        } while (changed);
        auto planned = planLayouts(limits, graph, analysis, pass_programs, resource_names);
        if (!planned)
        {
            return cxx::unexpected(planned.error());
        }
        result.descriptor_plan_ = std::make_unique<LayoutPlan>(std::move(*planned));
        const auto& layout_plan = *result.descriptor_plan_;
        if (!layout_plan.shared_bindings.empty() && (!sampled_image_capacity || !sampler_capacity))
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
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
                return cxx::unexpected(RenderError{kDeviceArgument, {id.index}});
            }
            const bool is_unsupported_pass = pass.conditional || pass.uses.empty();
            if (is_unsupported_pass)
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported, {id.index}});
            }
            if (!std::ranges::all_of(binding->clear_color, [](float x) { return std::isfinite(x); }))
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            CompiledPass compiled{};
            compiled.layout_program = id.index;
            compiled.clear_color = binding->clear_color;
            if (!std::isfinite(binding->clear_depth) || binding->clear_depth < 0 || binding->clear_depth > 1)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            compiled.clear_depth = binding->clear_depth;
            compiled.dispatch = binding->dispatch;
            const bool compute = pass.kind == EGraphPassKind::COMPUTE;
            if (compute)
            {
                if (queue_type == nri::QueueType::COPY)
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    if (!binding->dispatch[axis] ||
                        binding->dispatch[axis] > limits.shaderStage.compute.dispatchMaxDim[axis])
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
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
                    return cxx::unexpected(RenderError{kDeviceUnsupported, {id.index, index}});
                }
                if (use.usage == EGraphUse::COLOR_ATTACHMENT || use.usage == EGraphUse::DEPTH_ATTACHMENT)
                {
                    auto& attachment = use.usage == EGraphUse::COLOR_ATTACHMENT ? compiled.color : compiled.depth;
                    if (attachment != kGraphInvalidIndex)
                    {
                        return cxx::unexpected(RenderError{kDeviceUnsupported});
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
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                const bool storage_use = use.usage == EGraphUse::STORAGE_READ ||
                                         use.usage == EGraphUse::STORAGE_WRITE ||
                                         use.usage == EGraphUse::STORAGE_READ_WRITE;
                if (use.usage == EGraphUse::SAMPLED || storage_use)
                {
                    const bool is_image = std::holds_alternative<GraphImageDesc>(resource.description);
                    const bool is_invalid_binding = (resource.origin == EGraphOrigin::IMPORTED && !resource.storage &&
                                                     resource.context_data == kGraphInvalidIndex) ||
                                                    (is_image && resource.origin != EGraphOrigin::IMPORTED) ||
                                                    is_image != (use.usage == EGraphUse::SAMPLED);
                    if (is_invalid_binding)
                    {
                        return cxx::unexpected(RenderError{kStorageContract});
                    }
                    if (is_image)
                    {
                        resource.image_usage = nri::TextureUsageBits::SHADER_RESOURCE;
                        // Published storage images already have Graphics ownership and SHADER_READ layout.
                        used[index] = true;
                        previous[index] = accessFor(use);
                        continue;
                    }
                }
                if (resource.origin == EGraphOrigin::IMPORTED &&
                    std::holds_alternative<GraphImageDesc>(resource.description))
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                auto current = accessFor(use);
                if (storage_use)
                {
                    current.stage = nri::StageBits::ALL; // Accumulate an exact shader mask from zero.
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
                            current.stage |= nri::StageBits::VERTEX_SHADER;
                        }
                        if (stages & 2)
                        {
                            current.stage |= nri::StageBits::FRAGMENT_SHADER;
                        }
                        if (stages & 4)
                        {
                            current.stage |= nri::StageBits::COMPUTE_SHADER;
                        }
                    }
                }
                const auto source = previous[index];
                if (std::holds_alternative<GraphImageDesc>(resource.description))
                {
                    const auto image_format = imageFormat(std::get<GraphImageDesc>(resource.description).format);
                    const bool clear = pass.kind == EGraphPassKind::TRANSFER &&
                                       use.usage == EGraphUse::TRANSFER_DESTINATION &&
                                       std::ranges::none_of(
                                           pass.uses,
                                           [](const auto& item) { return item.access == EGraphAccess::READ; }
                                       );
                    if (clear)
                    {
                        current =
                            image_format.aspects == nri::PlaneBits::DEPTH
                                ? Access{nri::StageBits::DEPTH_STENCIL_ATTACHMENT, nri::AccessBits::DEPTH_STENCIL_ATTACHMENT, nri::Layout::DEPTH_STENCIL_ATTACHMENT}
                                : Access{
                                      nri::StageBits::COLOR_ATTACHMENT,
                                      nri::AccessBits::COLOR_ATTACHMENT_WRITE,
                                      nri::Layout::COLOR_ATTACHMENT
                                  };
                    }
                    nri::TextureBarrierDesc barrier{};
                    barrier.before = {source.access, source.layout, source.stage};
                    barrier.after = {current.access, current.layout, current.stage};
                    barrier.mipNum = barrier.layerNum = 1;
                    barrier.planes = image_format.aspects;
                    compiled.images.push_back({index, barrier});
                    if (use.usage == EGraphUse::COLOR_ATTACHMENT ||
                        (clear && image_format.aspects == nri::PlaneBits::COLOR))
                    {
                        resource.image_usage |= nri::TextureUsageBits::COLOR_ATTACHMENT;
                    }
                    if (use.usage == EGraphUse::DEPTH_ATTACHMENT ||
                        (clear && image_format.aspects == nri::PlaneBits::DEPTH))
                    {
                        resource.image_usage |= nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT;
                    }
                    if (use.access == EGraphAccess::ATTACHMENT_LOAD)
                    {
                        auto& load = use.usage == EGraphUse::DEPTH_ATTACHMENT ? compiled.depth_load : compiled.load;
                        load = nri::LoadOp::LOAD;
                    }
                }
                else
                {
                    nri::BufferBarrierDesc barrier{};
                    barrier.before = {source.access, source.stage};
                    if (!used[index])
                    {
                        // First-use execution order also covers a reused slice's prior reads and writes.
                        // CPU visibility for uploads is established at their explicit flush/submit boundary.
                        barrier.before = {nri::AccessBits::NONE, nri::StageBits::ALL};
                    }
                    barrier.after = {current.access, current.stage};
                    compiled.buffers.push_back({index, barrier});
                    resource.buffer_usage |= storage_use ? nri::BufferUsageBits::SHADER_RESOURCE_STORAGE
                                             : use.usage == EGraphUse::VERTEX   ? nri::BufferUsageBits::VERTEX_BUFFER
                                             : use.usage == EGraphUse::INDEX    ? nri::BufferUsageBits::INDEX_BUFFER
                                             : use.usage == EGraphUse::INDIRECT ? nri::BufferUsageBits::ARGUMENT_BUFFER
                                                                                : nri::BufferUsageBits::NONE;
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
                return cxx::unexpected(RenderError{kDeviceUnsupported});
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
                    auto lowered = lowerDraw(*binding, pass, result.resources_, resource_names, limits, compiled);
                    if (!lowered)
                    {
                        return cxx::unexpected(lowered.error());
                    }
                }
                if (!result.layout_)
                {
                    const auto shared_ranges =
                        descriptorRanges(layout_plan.shared_bindings, sampled_image_capacity, sampler_capacity);
                    const auto private_ranges = descriptorRanges(layout_plan.private_bindings, 0, 0);
                    std::vector<nri::DescriptorSetDesc> sets;
                    if (!shared_ranges.empty())
                    {
                        sets.push_back(
                            {layout_plan.shared_set,
                             shared_ranges.data(),
                             std::uint32_t(shared_ranges.size()),
                             nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET}
                        );
                    }
                    if (!private_ranges.empty())
                    {
                        sets.push_back(
                            {layout_plan.private_set, private_ranges.data(), std::uint32_t(private_ranges.size())}
                        );
                    }
                    const nri::RootConstantDesc parameters{
                        0,
                        layout_plan.parameter_bytes,
                        layout_plan.parameter_stages
                    };
                    nri::PipelineLayoutDesc description{};
                    description.rootRegisterSpace = static_cast<std::uint32_t>(sets.size());
                    description.rootConstants = &parameters;
                    description.rootConstantNum = layout_plan.parameter_bytes != 0;
                    description.descriptorSets = sets.data();
                    description.descriptorSetNum = static_cast<std::uint32_t>(sets.size());
                    description.shaderStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER |
                                               nri::StageBits::COMPUTE_SHADER;
                    description.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
                    const auto created = api.CreatePipelineLayout(device, description, result.layout_);
                    if (created != nri::Result::SUCCESS)
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
                PipelineIdentity identity{std::move(*shader), compute ? GraphicsState{} : binding->graphics};
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
                const auto same = std::ranges::find(pipeline_identities, identity);
                if (same != pipeline_identities.end())
                {
                    compiled.pipeline = result.pipelines_[same - pipeline_identities.begin()];
                }
                else
                {
                    auto pipeline = compute ? compileCompute(device, api, *result.layout_, identity.shader)
                                            : compileGraphics(
                                                  device,
                                                  api,
                                                  enabled,
                                                  formats,
                                                  vertex_limits,
                                                  *result.layout_,
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
                    pipeline_identities.push_back(std::move(identity));
                }
            }
            else if (compiled.source == kGraphInvalidIndex)
            {
                if (!writes_image &&
                    std::get<GraphBufferDesc>(result.resources_[compiled.destination].description).size % 4 != 0)
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                compiled.recipe = writes_image ? ERecipe::CLEAR_IMAGE : ERecipe::FILL_BUFFER;
            }
            else
            {
                if (writes_image)
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
                const auto& source = result.resources_[compiled.source];
                auto& destination = result.resources_[compiled.destination];
                const auto size = std::get<GraphBufferDesc>(destination.description).size;
                if (const auto* image = std::get_if<GraphImageDesc>(&source.description))
                {
                    if (size != std::uint64_t(image->width) * image->height * imageFormat(image->format).bytes)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    compiled.recipe = ERecipe::COPY_IMAGE_BUFFER;
                }
                else
                {
                    if (size != std::get<GraphBufferDesc>(source.description).size)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    compiled.recipe = ERecipe::COPY_BUFFER;
                }
            }
            if (result.pass_indices_.size() <= id.index)
            {
                result.pass_indices_.resize(std::size_t(id.index) + 1, kGraphInvalidIndex);
            }
            result.pass_indices_[id.index] = static_cast<std::uint32_t>(result.passes_.size());
            result.passes_.push_back(std::move(compiled));
        }
        // Lower the fixed legal execution order to static record decisions. No Pass reordering.
        nri::Pipeline* bound = nullptr;
        for (std::size_t i = 0; i < result.passes_.size(); ++i)
        {
            auto& current = result.passes_[i];
            if (current.recipe != ERecipe::DRAW)
            {
                if (current.recipe == ERecipe::DISPATCH)
                {
                    bound = nullptr; // NRI tracks the currently selected pipeline across bind points.
                }
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
            const bool preserves_content = current.load == nri::LoadOp::LOAD;
            const bool attachment_only = current.images.size() == 1 && current.buffers.empty() &&
                                         current.images[0].native.before.layout == nri::Layout::COLOR_ATTACHMENT;
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
            if (used[i] && resource.origin == EGraphOrigin::IMPORTED && !resource.storage &&
                resource.context_data == kGraphInvalidIndex)
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            }
            if (used[i] && std::holds_alternative<GraphImageDesc>(resource.description))
            {
                const auto format = imageFormat(std::get<GraphImageDesc>(resource.description).format).native;
                auto required = nri::FormatSupportBits::UNSUPPORTED;
                if ((resource.image_usage & nri::TextureUsageBits::COLOR_ATTACHMENT) != 0)
                {
                    required |= nri::FormatSupportBits::COLOR_ATTACHMENT;
                }
                if ((resource.image_usage & nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT) != 0)
                {
                    required |= nri::FormatSupportBits::DEPTH_STENCIL_ATTACHMENT;
                }
                if ((resource.image_usage & nri::TextureUsageBits::SHADER_RESOURCE) != 0)
                {
                    required |= nri::FormatSupportBits::TEXTURE;
                }
                const auto supported = formats[static_cast<std::size_t>(format)];
                if (supported == nri::FormatSupportBits::UNSUPPORTED ||
                    (supported & required) != static_cast<std::uint16_t>(required))
                {
                    return cxx::unexpected(RenderError{kDeviceUnsupported});
                }
            }
        }
        for (std::uint32_t pass = 0; pass < result.passes_.size(); ++pass)
        {
            const auto& recipe = result.passes_[pass];
            for (std::uint32_t index = 0; index < recipe.buffers.size(); ++index)
            {
                result.resources_[recipe.buffers[index].resource].barrier_uses.emplace_back(pass, index);
            }
            if (recipe.recipe != ERecipe::DRAW && recipe.recipe != ERecipe::DISPATCH)
            {
                continue;
            }
            const auto& layout = result.descriptor_plan_->programs[recipe.layout_program];
            for (std::uint32_t index = 0; index < layout.uses.size(); ++index)
            {
                const auto& use = layout.uses[index];
                if (use.kind == EDescriptorKind::STORAGE_BUFFER)
                {
                    result.resources_[use.resource].descriptor_uses.emplace_back(pass, index);
                }
            }
        }
        return result;
    }
} // namespace lux::render::detail
