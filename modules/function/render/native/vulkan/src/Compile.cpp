#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include "Shader.hpp"
#include "Native.hpp"
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace lux::render::vulkan
{
    CompiledGraph::~CompiledGraph() noexcept
    {
        if (!device_) return;
        for (auto pipeline : pipelines_)
            LUX_DESTROY("pipeline", vkDestroyPipeline(device_, pipeline, nullptr));
        if (layout_) LUX_DESTROY("pipeline_layout", vkDestroyPipelineLayout(device_, layout_, nullptr));
    }

    CompiledGraph::CompiledGraph(CompiledGraph&& other) noexcept
        : device_(std::exchange(other.device_, VK_NULL_HANDLE)), target_(other.target_), layout_(other.layout_),
          resources_(std::move(other.resources_)), passes_(std::move(other.passes_)),
          host_barriers_(std::move(other.host_barriers_)), shader_identities_(std::move(other.shader_identities_)),
          pipelines_(std::move(other.pipelines_)), output_(other.output_)
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
            case EGraphUse::SAMPLED:
                return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            default:
                return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
            }
        }

        bool fullRange(const GraphResourceUse& use, const ResourcePlan& resource) noexcept
        {
            if (auto* b = std::get_if<GraphBufferDesc>(&resource.description))
                return std::get<GraphBufferRange>(use.range) == GraphBufferRange{0, b->size};
            const auto& image = std::get<GraphImageDesc>(resource.description);
            return std::get<GraphImageRange>(use.range) == GraphImageRange{1, 0, image.mips, 0, image.layers};
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
        const auto& device = session.device();
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
                const bool is_imported = resource.origin == EGraphOrigin::IMPORTED;
                const bool sampled_format = image->format == rdesc::ETextureFormat::RGBA8_UNORM ||
                    image->format == rdesc::ETextureFormat::RGBA8_SRGB || image->format == rdesc::ETextureFormat::BC1_RGBA_SRGB;
                const bool is_unsupported_image = image->layers != 1 || image->samples != 1 ||
                    (is_imported ? !sampled_format : image->mips != 1 ||
                        image->format != rdesc::ETextureFormat::RGBA8_UNORM || resource.origin != EGraphOrigin::TRANSIENT);
                if (is_unsupported_image) return cxx::unexpected(RenderError{kUnsupported});
            }
            result.resources_.push_back({resource.key, resource.description, resource.origin});
        }
        for (const auto& binding : assembly.external_bindings)
        {
            auto& resource = result.resources_[binding.resource.index];
            const bool is_image = std::holds_alternative<GraphImageDesc>(resource.description);
            const bool invalid_texture = is_image &&
                (binding.storage.type.name() != kTextureStorageType.name() ||
                 binding.storage.scope != EResourceScope::GLOBAL || resource.origin != EGraphOrigin::IMPORTED);
            const bool is_unsupported_storage = invalid_texture || binding.storage.abi != cxx::SchemaVersion{1, 0};
            if (is_unsupported_storage) return cxx::unexpected(RenderError{kUnsupported});
            resource.storage = binding.storage;
        }
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
        const bool sampled_program = std::ranges::any_of(programs, [](const auto& p) { return p.shader.abi == 2; });
        if (sampled_program && !session.textures()) return cxx::unexpected(RenderError{kUnsupported});
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
            compiled.texture_lod = binding->texture_lod;
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
                if (use.usage == EGraphUse::SAMPLED)
                {
                    const auto* image = std::get_if<GraphImageDesc>(&resource.description);
                    const bool invalid_texture = !image || resource.origin != EGraphOrigin::IMPORTED || !resource.storage ||
                        binding->shader.abi != 2 || binding->texture.isNull() ||
                        (!resource.texture.isNull() && resource.texture != binding->texture) ||
                        !std::isfinite(binding->texture_lod) || binding->texture_lod < 0 ||
                        std::floor(binding->texture_lod) != binding->texture_lod ||
                        (image && binding->texture_lod > image->mips - 1);
                    if (invalid_texture) return cxx::unexpected(RenderError{kInvalidArgument});
                    resource.texture = binding->texture;
                    resource.image_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
                    // Native publication already established Graphics ownership and SHADER_READ layout.
                    // Immutable read/read sampling needs no recurring image transition or binding lookup.
                    used[index] = true;
                    previous[index] = accessFor(use);
                    continue;
                }
                if (resource.origin == EGraphOrigin::IMPORTED && std::holds_alternative<GraphImageDesc>(resource.description))
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
                const bool valid_sample = binding->shader.abi == 2 && reads == 1 &&
                    !result.resources_[compiled.source].texture.isNull();
                const bool valid_color = binding->shader.abi == 1 && reads == 0 && binding->texture.isNull();
                if ((!valid_sample && !valid_color) || !writes_image || binding->vertex_count == 0)
                    return cxx::unexpected(RenderError{kUnsupported});
                compiled.recipe = ERecipe::DRAW;
                if (!result.layout_)
                {
                    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
                    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                    info.pushConstantRangeCount = 1;
                    info.pPushConstantRanges = &range;
                    const auto descriptors = sampled_program ? session.textures()->layout() : VK_NULL_HANDLE;
                    info.setLayoutCount = sampled_program ? 1 : 0;
                    info.pSetLayouts = sampled_program ? &descriptors : nullptr;
                    const auto created = LUX_NATIVE("pipeline_layout", vkCreatePipelineLayout(device.native(), &info,
                        nullptr, &result.layout_));
                    if (created != VK_SUCCESS) return cxx::unexpected(nativeError(created));
                }
                // This graph has one fixed RGBA8/push-color layout and raster state. Compare complete
                // stage bytes + ABI; draw count and clear color are not Pipeline identity.
                const auto same = std::find(result.shader_identities_.begin(), result.shader_identities_.end(),
                    binding->shader);
                if (same != result.shader_identities_.end())
                    compiled.pipeline = result.pipelines_[same - result.shader_identities_.begin()];
                else
                {
                    auto pipeline = compileGraphics(device, result.layout_, binding->shader);
                    if (!pipeline) return cxx::unexpected(pipeline.error());
                    compiled.pipeline = *pipeline;
                    result.pipelines_.push_back(*pipeline);
                    result.shader_identities_.push_back(binding->shader);
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
