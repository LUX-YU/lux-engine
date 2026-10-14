#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include "Shader.hpp"
#include "Native.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace lux::render::vulkan
{
    CompiledGraph::~CompiledGraph() noexcept
    {
        if (!device_) return;
        for (const auto& pass : passes_)
            if (pass.pipeline) LUX_DESTROY("pipeline", vkDestroyPipeline(device_, pass.pipeline, nullptr));
        if (layout_) LUX_DESTROY("pipeline_layout", vkDestroyPipelineLayout(device_, layout_, nullptr));
    }

    CompiledGraph::CompiledGraph(CompiledGraph&& other) noexcept
        : device_(std::exchange(other.device_, VK_NULL_HANDLE)), target_(other.target_), layout_(other.layout_),
          resources_(std::move(other.resources_)), passes_(std::move(other.passes_)),
          host_barriers_(std::move(other.host_barriers_)), shader_identities_(std::move(other.shader_identities_)),
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
            default:
                return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
            }
        }

        bool fullRange(const GraphResourceUse& use, const ResourcePlan& resource) noexcept
        {
            if (auto* b = std::get_if<GraphBufferDesc>(&resource.description))
                return std::get<GraphBufferRange>(use.range) == GraphBufferRange{0, b->size};
            return std::get<GraphImageRange>(use.range) == GraphImageRange{1, 0, 1, 0, 1};
        }
    }

    RenderResult<CompiledGraph> compileGraph(
        const DeviceSession& session,
        const FeatureAssembly& assembly,
        std::span<const PassProgram> programs,
        const TargetDesc& target
    ) noexcept
    {
        if (auto result = target.validate(); !result) return cxx::unexpected(result.error());
        const auto& device = session.device;
        const bool is_unsupported_target = target.kind != ETargetKind::OFFSCREEN || target.samples != 1 ||
            target.format != rdesc::ETextureFormat::RGBA8_UNORM ||
            target.width > device.properties().limits.maxImageDimension2D ||
            target.height > device.properties().limits.maxImageDimension2D;
        if (is_unsupported_target) return cxx::unexpected(RenderError{kUnsupported});
        const auto& graph = assembly.definition;
        CompiledGraph result(device.native(), target);
        for (const auto& resource : graph.resources())
        {
            const bool is_history = resource.origin == EGraphOrigin::HISTORY_PREVIOUS ||
                resource.origin == EGraphOrigin::HISTORY_CURRENT;
            if (is_history) return cxx::unexpected(RenderError{kUnsupported});
            if (const auto* image = std::get_if<GraphImageDesc>(&resource.description))
            {
                const bool is_unsupported_image = image->mips != 1 || image->layers != 1 || image->samples != 1 ||
                    image->format != rdesc::ETextureFormat::RGBA8_UNORM ||
                    resource.origin != EGraphOrigin::TRANSIENT;
                if (is_unsupported_image) return cxx::unexpected(RenderError{kUnsupported});
            }
            result.resources_.push_back({resource.key, resource.description, resource.origin});
        }
        for (const auto& binding : assembly.external_bindings)
        {
            auto& resource = result.resources_[binding.resource.index];
            const bool is_unsupported_storage = !std::holds_alternative<GraphBufferDesc>(resource.description) ||
                binding.storage.abi != cxx::SchemaVersion{1, 0};
            if (is_unsupported_storage) return cxx::unexpected(RenderError{kUnsupported});
            resource.storage = binding.storage;
        }
        for (const auto& output : graph.outputs())
        {
            auto& plan = result.resources_[output.version.resource.index];
            if (std::holds_alternative<GraphBufferDesc>(plan.description)) plan.readback = true;
            if (!output.target) continue;
            if (*output.target != target || result.output_ != kGraphInvalidIndex)
                return cxx::unexpected(RenderError{kInvalidArgument});
            result.output_ = output.version.resource.index;
            plan.target_sized = true;
        }
        if (result.output_ == kGraphInvalidIndex) return cxx::unexpected(RenderError{kInvalidArgument});
        for (std::size_t i = 0; i < programs.size(); ++i)
        {
            for (std::size_t j = 0; j < i; ++j)
                if (programs[i].pass_key == programs[j].pass_key)
                    return cxx::unexpected(RenderError{kInvalidArgument});
        }
        std::vector<Access> previous(result.resources_.size());
        std::vector<bool> used(result.resources_.size(), false);
        // INVARIANT: exact G5 analysis determines order; no graph reconstruction, pruning or re-analysis.
        for (auto id : assembly.analysis.execution_order)
        {
            const auto& pass = graph.passes()[id.index];
            const auto binding = std::find_if(programs.begin(), programs.end(), [&](const auto& p) {
                return p.pass_key == pass.key;
            });
            if (binding == programs.end()) return cxx::unexpected(RenderError{kFeatureContract, {id.index}});
            const bool is_unsupported_pass = pass.conditional || pass.kind == EGraphPassKind::COMPUTE ||
                pass.uses.empty() || pass.uses.size() > 2;
            if (is_unsupported_pass) return cxx::unexpected(RenderError{kUnsupported, {id.index}});
            if (!std::ranges::all_of(binding->clear_color, [](float x) { return std::isfinite(x); }))
                return cxx::unexpected(RenderError{kInvalidArgument});
            CompiledPass compiled{};
            compiled.clear_color = binding->clear_color;
            compiled.vertex_count = binding->vertex_count;
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
                const auto current = accessFor(use);
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
                    if (!used[index] && resource.origin == EGraphOrigin::IMPORTED)
                    {
                        barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                        barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
                    }
                    barrier.dstStageMask = current.stage;
                    barrier.dstAccessMask = current.access;
                    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.size = VK_WHOLE_SIZE;
                    compiled.buffers.push_back({index, barrier});
                    resource.buffer_usage |= use.usage == EGraphUse::TRANSFER_SOURCE ?
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                }
                used[index] = true;
                previous[index] = current;
            }
            const bool is_unsupported_recipe = writes != 1 || reads > 1 ||
                (pass.kind == EGraphPassKind::TRANSFER &&
                 (!binding->shader.vertex.empty() || !binding->shader.fragment.empty()));
            if (is_unsupported_recipe) return cxx::unexpected(RenderError{kUnsupported});
            const bool writes_image = std::holds_alternative<GraphImageDesc>(
                result.resources_[compiled.destination].description);
            if (pass.kind == EGraphPassKind::GRAPHICS)
            {
                if (pass.uses.size() != 1 || binding->vertex_count == 0)
                    return cxx::unexpected(RenderError{kUnsupported});
                compiled.recipe = ERecipe::DRAW;
                if (!result.layout_)
                {
                    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
                    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                    info.pushConstantRangeCount = 1;
                    info.pPushConstantRanges = &range;
                    const auto created = LUX_NATIVE("pipeline_layout", vkCreatePipelineLayout(device.native(), &info,
                        nullptr, &result.layout_));
                    if (created != VK_SUCCESS) return cxx::unexpected(nativeError(created));
                }
                auto pipeline = compileGraphics(device, result.layout_, binding->shader);
                if (!pipeline) return cxx::unexpected(pipeline.error());
                compiled.pipeline = *pipeline;
                result.shader_identities_.push_back(binding->shader);
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
