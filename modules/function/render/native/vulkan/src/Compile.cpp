#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include "Shader.hpp"
#include "LayoutPlan.hpp"
#include "Native.hpp"
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace lux::render::vulkan
{
    CompiledGraph::CompiledGraph(VkDevice device, TargetDesc target) noexcept : device_(device), target_(target) {}

    CompiledGraph::~CompiledGraph() noexcept
    {
        if (!device_) return;
        for (auto pipeline : pipelines_)
            LUX_DESTROY("pipeline", vkDestroyPipeline(device_, pipeline, nullptr));
        if (layout_) LUX_DESTROY("pipeline_layout", vkDestroyPipelineLayout(device_, layout_, nullptr));
        for (const auto layout : descriptor_layouts_)
            LUX_DESTROY("shader_layout", vkDestroyDescriptorSetLayout(device_, layout, nullptr));
    }

    CompiledGraph::CompiledGraph(CompiledGraph&& other) noexcept
        : descriptor_plan_(std::move(other.descriptor_plan_)), device_(std::exchange(other.device_, VK_NULL_HANDLE)), target_(other.target_), layout_(other.layout_),
          resources_(std::move(other.resources_)), storage_groups_(std::move(other.storage_groups_)), passes_(std::move(other.passes_)),
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
                return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                        (use.access == EGraphAccess::ATTACHMENT_LOAD ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : 0),
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            case EGraphUse::TRANSFER_SOURCE:
                return {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
            case EGraphUse::STORAGE_WRITE:
                return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
            case EGraphUse::STORAGE_READ_WRITE:
                return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
            case EGraphUse::STORAGE_READ:
                return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
            case EGraphUse::SAMPLED:
                return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            default:
                return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
            }
        }

        bool fullRange(const GraphResourceUse& use, const NativeResourcePlan& resource) noexcept
        {
            if (auto* b = std::get_if<GraphBufferDesc>(&resource.description))
                return std::get<GraphBufferRange>(use.range) == GraphBufferRange{0, b->size};
            const auto& image = std::get<GraphImageDesc>(resource.description);
            return std::get<GraphImageRange>(use.range) == GraphImageRange{1, 0, image.mips, 0, image.layers};
        }
    }

    RenderResult<CompiledGraph> compileGraph(
        const DeviceSession& session,
        const RenderGraphDefinition& graph,
        const GraphAnalysis& analysis,
        const GraphStorageBindings& external_bindings,
        std::span<const PassProgram> programs,
        const TargetDesc& target
    ) noexcept
    {
        if (auto result = target.validate(); !result) return cxx::unexpected(result.error());
        const auto& device = session.device();
        const bool is_unsupported_target = target.kind != ETargetKind::OFFSCREEN || target.samples != 1 ||
            target.format != rdesc::ETextureFormat::RGBA8_UNORM ||
            target.width > device.properties().limits.maxImageDimension2D ||
            target.height > device.properties().limits.maxImageDimension2D;
        if (is_unsupported_target) return cxx::unexpected(RenderError{kUnsupported});
        CompiledGraph result(device.native(), target);
        // Resolve author keys once at the cold compiler boundary, then index by GraphPassId.
        std::vector<const PassProgram*> pass_programs(graph.passes().size(), nullptr);
        for (const auto& program : programs)
        {
            const auto found = std::ranges::find(graph.passes(), program.pass_key, &GraphPass::key);
            if (found == graph.passes().end()) return cxx::unexpected(RenderError{kInvalidArgument});
            const auto index = static_cast<std::size_t>(found - graph.passes().begin());
            if (pass_programs[index]) return cxx::unexpected(RenderError{kInvalidArgument});
            pass_programs[index] = &program;
        }
        std::vector<bool> live_resources(graph.resources().size(), false);
        for (const auto pass : analysis.execution_order)
            for (const auto& use : graph.passes()[pass.index].uses) live_resources[use.version.resource.index] = true;
        for (const auto& output : graph.outputs()) live_resources[output.version.resource.index] = true;
        for (const auto& resource : graph.resources())
        {
            const auto index = result.resources_.size();
            result.resources_.push_back({resource.key, resource.description, resource.origin});
            if (!live_resources[index]) continue;
            const bool is_history = resource.origin == EGraphOrigin::HISTORY_PREVIOUS ||
                resource.origin == EGraphOrigin::HISTORY_CURRENT;
            if (is_history) return cxx::unexpected(RenderError{kUnsupported});
            if (const auto* image = std::get_if<GraphImageDesc>(&resource.description))
            {
                const bool is_imported = resource.origin == EGraphOrigin::IMPORTED;
                const bool sampled_format = image->format == rdesc::ETextureFormat::RGBA8_UNORM ||
                    image->format == rdesc::ETextureFormat::RGBA8_SRGB || image->format == rdesc::ETextureFormat::BC1_RGBA_SRGB;
                const bool is_unsupported_image = image->layers != 1 || image->samples != 1 ||
                    (is_imported ? !sampled_format : image->mips != 1 ||
                        image->format != rdesc::ETextureFormat::RGBA8_UNORM || resource.origin != EGraphOrigin::TRANSIENT);
                if (is_unsupported_image) return cxx::unexpected(RenderError{kUnsupported});
            }
        }
        const auto install = [&](std::span<const GraphStorageBinding> source,
            std::vector<std::uint32_t>& indices) -> RenderResult<void> {
            for (const auto& binding : source)
            {
                if (binding.resource.index >= result.resources_.size())
                    return cxx::unexpected(RenderError{kInvalidArgument});
                if (!live_resources[binding.resource.index]) continue;
                auto& resource = result.resources_[binding.resource.index];
                if (resource.storage) return cxx::unexpected(RenderError{kInvalidArgument});
                const auto* factory = session.resources().factory(binding.storage.type.view());
                if (!factory || !factory->binding_ops ||
                    !static_cast<const StorageBindingOps*>(factory->binding_ops)->prepare)
                    return cxx::unexpected(RenderError{kStorageFactory});
                resource.storage = binding.storage;
                indices.push_back(binding.resource.index);
            }
            return {};
        };
        if (auto r = install(external_bindings.global, result.storage_groups_.global); !r) return cxx::unexpected(r.error());
        if (auto r = install(external_bindings.scene, result.storage_groups_.scene); !r) return cxx::unexpected(r.error());
        if (auto r = install(external_bindings.view, result.storage_groups_.view); !r) return cxx::unexpected(r.error());
        if (auto r = install(external_bindings.attachment, result.storage_groups_.attachment); !r)
            return cxx::unexpected(r.error());
        for (const auto& output : graph.outputs())
        {
            auto& plan = result.resources_[output.version.resource.index];
            if (std::holds_alternative<GraphBufferDesc>(plan.description)) plan.readback = true;
            if (!output.target) continue;
            if (plan.origin != EGraphOrigin::TRANSIENT) return cxx::unexpected(RenderError{kUnsupported});
            if (*output.target != target || result.output_ != kGraphInvalidIndex)
                return cxx::unexpected(RenderError{kInvalidArgument});
            result.output_ = output.version.resource.index;
            plan.target_sized = true;
        }
        if (result.output_ == kGraphInvalidIndex) return cxx::unexpected(RenderError{kInvalidArgument});
        auto planned = planLayouts(device, graph, analysis, pass_programs);
        if (!planned) return cxx::unexpected(planned.error());
        result.descriptor_plan_ = std::make_unique<LayoutPlan>(std::move(*planned));
        const auto& layout_plan = *result.descriptor_plan_;
        if (!layout_plan.shared_bindings.empty())
        {
            auto capacity = session.sampledImageCapacity();
            if (!capacity) return cxx::unexpected(capacity.error());
            auto layout = createDescriptorLayout(device, layout_plan.shared_bindings, *capacity, "shader_layout");
            if (!layout) return cxx::unexpected(layout.error());
            result.descriptor_layouts_.push_back(*layout);
        }
        if (!layout_plan.private_bindings.empty())
        {
            auto layout = createDescriptorLayout(device, layout_plan.private_bindings, 0, "shader_layout");
            if (!layout) return cxx::unexpected(layout.error());
            result.descriptor_layouts_.push_back(*layout);
        }
        std::vector<Access> previous(result.resources_.size());
        std::vector<bool> used(result.resources_.size(), false);
        // INVARIANT: exact Graph analysis determines order; no graph reconstruction, pruning or re-analysis.
        for (auto id : analysis.execution_order)
        {
            const auto& pass = graph.passes()[id.index];
            const auto* binding = pass_programs[id.index];
            if (!binding) return cxx::unexpected(RenderError{kInvalidArgument, {id.index}});
            const bool is_unsupported_pass = pass.conditional || pass.uses.empty();
            if (is_unsupported_pass) return cxx::unexpected(RenderError{kUnsupported, {id.index}});
            if (!std::ranges::all_of(binding->clear_color, [](float x) { return std::isfinite(x); }))
                return cxx::unexpected(RenderError{kInvalidArgument});
            CompiledPass compiled{};
            compiled.layout_program = id.index;
            compiled.clear_color = binding->clear_color;
            compiled.vertex_count = binding->vertex_count;
            compiled.dispatch = binding->dispatch;
            const bool compute = pass.kind == EGraphPassKind::COMPUTE;
            if (compute)
            {
                if (!(device.queue(EQueueRole::GRAPHICS).flags & VK_QUEUE_COMPUTE_BIT))
                    return cxx::unexpected(RenderError{kUnsupported});
                for (unsigned axis = 0; axis < 3; ++axis)
                    if (!binding->dispatch[axis] ||
                        binding->dispatch[axis] > device.properties().limits.maxComputeWorkGroupCount[axis])
                        return cxx::unexpected(RenderError{kInvalidArgument});
            }
            unsigned reads = 0, writes = 0;
            for (const auto& use : pass.uses)
            {
                const auto index = use.version.resource.index;
                auto& resource = result.resources_[index];
                const bool is_unsupported_use = use.usage == EGraphUse::UNSPECIFIED || use.fallback ||
                    !fullRange(use, resource);
                if (is_unsupported_use) return cxx::unexpected(RenderError{kUnsupported, {id.index, index}});
                if (use.access == EGraphAccess::READ) { compiled.source = index; ++reads; }
                else { compiled.destination = index; ++writes; }
                if (resource.origin == EGraphOrigin::IMPORTED && use.access != EGraphAccess::READ)
                    return cxx::unexpected(RenderError{kUnsupported});
                const bool storage_use = use.usage == EGraphUse::STORAGE_READ ||
                    use.usage == EGraphUse::STORAGE_WRITE || use.usage == EGraphUse::STORAGE_READ_WRITE;
                if (use.usage == EGraphUse::SAMPLED || storage_use)
                {
                    const bool is_image = std::holds_alternative<GraphImageDesc>(resource.description);
                    const bool is_invalid_binding = (resource.origin == EGraphOrigin::IMPORTED && !resource.storage) ||
                        (is_image && resource.origin != EGraphOrigin::IMPORTED) ||
                        is_image != (use.usage == EGraphUse::SAMPLED);
                    if (is_invalid_binding) return cxx::unexpected(RenderError{kStorageContract});
                    if (is_image)
                    {
                        resource.image_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
                        // Published storage images already have Graphics ownership and SHADER_READ layout.
                        used[index] = true;
                        previous[index] = accessFor(use);
                        continue;
                    }
                }
                if (resource.origin == EGraphOrigin::IMPORTED && std::holds_alternative<GraphImageDesc>(resource.description))
                    return cxx::unexpected(RenderError{kUnsupported});
                auto current = accessFor(use);
                if (storage_use)
                {
                    current.stage = VK_PIPELINE_STAGE_2_NONE;
                    const auto& program = layout_plan.programs[id.index];
                    for (std::size_t field = 0; field < program.uses.size(); ++field)
                    {
                        if (program.uses[field].resource != index) continue;
                        const auto stages = program.bindings[field].stages;
                        if (stages & 1) current.stage |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
                        if (stages & 2) current.stage |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                        if (stages & 4) current.stage |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
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
                    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    compiled.images.push_back({index, barrier});
                    resource.image_usage |= use.usage == EGraphUse::COLOR_ATTACHMENT ?
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : use.usage == EGraphUse::TRANSFER_SOURCE ?
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                    if (use.access == EGraphAccess::ATTACHMENT_LOAD) compiled.load = VK_ATTACHMENT_LOAD_OP_LOAD;
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
                        barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT |
                            VK_ACCESS_2_MEMORY_READ_BIT;
                    }
                    barrier.dstStageMask = current.stage;
                    barrier.dstAccessMask = current.access;
                    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.size = VK_WHOLE_SIZE;
                    compiled.buffers.push_back({index, barrier});
                    resource.buffer_usage |= storage_use ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT :
                        use.usage == EGraphUse::TRANSFER_SOURCE ?
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                }
                used[index] = true;
                previous[index] = current;
            }
            const bool is_unsupported_recipe = (!compute && writes != 1) || (compute && writes == 0) ||
                (pass.kind == EGraphPassKind::TRANSFER && reads > 1) ||
                (pass.kind == EGraphPassKind::TRANSFER &&
                 !binding->shader.stages.empty());
            if (is_unsupported_recipe) return cxx::unexpected(RenderError{kUnsupported});
            const bool writes_image = std::holds_alternative<GraphImageDesc>(
                result.resources_[compiled.destination].description);
            if (pass.kind == EGraphPassKind::GRAPHICS || compute)
            {
                if (!compute && (!writes_image || binding->vertex_count == 0))
                    return cxx::unexpected(RenderError{kStorageContract});
                compiled.recipe = compute ? ERecipe::DISPATCH : ERecipe::DRAW;
                if (!result.layout_)
                {
                    const VkPushConstantRange range{
                        layout_plan.parameter_stages, 0, layout_plan.parameter_bytes
                    };
                    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                    info.pushConstantRangeCount = unsigned(layout_plan.parameter_bytes != 0);
                    info.pPushConstantRanges = &range;
                    info.setLayoutCount = std::uint32_t(result.descriptor_layouts_.size());
                    info.pSetLayouts = result.descriptor_layouts_.data();
                    const auto created = LUX_NATIVE("pipeline_layout", vkCreatePipelineLayout(device.native(), &info,
                        nullptr, &result.layout_));
                    if (created != VK_SUCCESS) return cxx::unexpected(nativeError(created));
                }
                // The Graph owns one compatible physical descriptor/parameter layout. Compare complete
                // stage bytes + ABI; draw count and clear color are not Pipeline identity.
                auto shader = relocateShader(binding->shader, layout_plan.programs[id.index].bindings);
                if (!shader) return cxx::unexpected(shader.error());
                const auto same = std::find(result.shader_identities_.begin(), result.shader_identities_.end(),
                    *shader);
                if (same != result.shader_identities_.end())
                    compiled.pipeline = result.pipelines_[same - result.shader_identities_.begin()];
                else
                {
                    auto pipeline = compute ? compileCompute(device, result.layout_, *shader) :
                        compileGraphics(device, result.layout_, *shader);
                    if (!pipeline) return cxx::unexpected(pipeline.error());
                    compiled.pipeline = *pipeline;
                    result.pipelines_.push_back(*pipeline);
                    result.shader_identities_.push_back(std::move(*shader));
                }
            }
            else if (compiled.source == kGraphInvalidIndex)
            {
                if (!writes_image && std::get<GraphBufferDesc>(
                    result.resources_[compiled.destination].description).size % 4 != 0)
                    return cxx::unexpected(RenderError{kUnsupported});
                compiled.recipe = writes_image ? ERecipe::CLEAR_IMAGE : ERecipe::FILL_BUFFER;
            }
            else
            {
                if (writes_image) return cxx::unexpected(RenderError{kUnsupported});
                const auto& source = result.resources_[compiled.source];
                auto& destination = result.resources_[compiled.destination];
                const auto size = std::get<GraphBufferDesc>(destination.description).size;
                if (const auto* image = std::get_if<GraphImageDesc>(&source.description))
                {
                    if (size != std::uint64_t(image->width) * image->height * 4)
                        return cxx::unexpected(RenderError{kInvalidArgument});
                    compiled.recipe = ERecipe::COPY_IMAGE_BUFFER;
                    destination.target_sized = source.target_sized;
                }
                else
                {
                    if (size != std::get<GraphBufferDesc>(source.description).size)
                        return cxx::unexpected(RenderError{kInvalidArgument});
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
            if (current.recipe != ERecipe::DRAW) continue;
            current.bind_pipeline = bound != current.pipeline;
            bound = current.pipeline; // transfer commands and Begin/EndRendering do not invalidate this state
            if (i == 0) continue;
            auto& previous = result.passes_[i - 1];
            const bool same_attachment = previous.recipe == ERecipe::DRAW &&
                previous.destination == current.destination;
            const bool preserves_content = current.load == VK_ATTACHMENT_LOAD_OP_LOAD;
            const bool attachment_only = current.images.size() == 1 && current.buffers.empty() &&
                current.images[0].native.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            // Sampled inputs are immutable, disjoint Global images, with no read/read barriers.
            // Rasterization-order color attachment access preserves LOAD within one scope.
            const bool can_merge = same_attachment && preserves_content && attachment_only;
            if (!can_merge) continue;
            previous.end_rendering = false;
            current.begin_rendering = false;
            current.images.clear();
        }
        for (std::uint32_t i = 0; i < result.resources_.size(); ++i)
        {
            const auto& resource = result.resources_[i];
            if (used[i] && resource.origin == EGraphOrigin::IMPORTED && !resource.storage)
                return cxx::unexpected(RenderError{kUnsupported});
            if (!used[i] || !resource.readback) continue;
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
}
