#include <lux/engine/render/graph/RGVulkanRecorder.hpp>
#include <lux/engine/render/graph/vk_type_converter.hpp>
#include <lux/engine/render/graph/RGBarrierUtils.hpp>
#include <lux/engine/render/graph/RGTextureUtils.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>

namespace lux::render
{
    // ================================
    // Group extents computation (for dynamic rendering — no framebuffers needed)
    // ================================
    std::optional<RenderError>
    RGVulkanRecorder::computeGroupExtents(RGRecordContext& record_context, const RGCompiledGraph& graph, VkExtent2D extent)
    {
        const auto& groups = graph.render_pass_layout.groups;
        record_context.group_extents.assign(groups.size(), VkExtent2D{0, 0});
        record_context.group_layer_counts.assign(groups.size(), 1u);

        for (std::size_t group_index = 0; group_index < groups.size(); ++group_index)
        {
            const auto& group = groups[group_index];
            if (group.passes.empty())
                continue;

            const uint32_t pass_idx = group.passes[0].pass_index;
            const auto& cpass = graph.compiled_passes[pass_idx];
            const auto* desc = cpass.pass;
            if (!desc || desc->type != ERGPassType::GRAPHICS)
                continue;

            for (const auto& tex_ref : desc->textures)
            {
                if (tex_ref.role != lux::render::ETextureRole::COLOR_ATTACHMENT
                    && tex_ref.role != lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                    continue;

                const auto& res_desc = graph.original_graph.resources[tex_ref.resource.index];
                const auto* tex_desc = std::get_if<RGTextureDescription>(&res_desc.desc);
                if (tex_desc)
                {
                    // Slotted imported resources (e.g. backbuffer) use target extent directly.
                    if (res_desc.lifetime == ERGResourceLifetime::IMPORTED
                        && res_desc.import_info
                        && res_desc.import_info->slot.has_value())
                    {
                        record_context.group_extents[group_index] = extent;
                    }
                    else
                    {
                        record_context.group_extents[group_index] = resolveTextureExtent(*tex_desc, extent);
                    }
                    if (tex_desc->dimension == lux::rdesc::ETextureDimension::TEX_2D_ARRAY
                        && tex_desc->array_layers > record_context.group_layer_counts[group_index])
                    {
                        record_context.group_layer_counts[group_index] = tex_desc->array_layers;
                    }
                    break;
                }
            }
        }

        return std::nullopt;
    }

    std::optional<RenderError>
    RGVulkanRecorder::preCreateImageViews(
        RGRecordContext& record_context,
        const RGCompiledGraph& graph,
        const RGPhysicalResourceTable& physical_resources,
        uint32_t frames_in_flight)
    {
        const size_t resource_count = graph.original_graph.resources.size();
        record_context.per_frame_views.assign(
            resource_count, std::vector<VkImageView>(frames_in_flight, VK_NULL_HANDLE));
        record_context.per_frame_views_by_mip.assign(resource_count, {});
        record_context.per_frame_images.assign(
            resource_count, std::vector<VkImage>(frames_in_flight, VK_NULL_HANDLE));

        VkDevice device = context_.logicalDevice();

        for (const auto& cpass : graph.compiled_passes)
        {
            if (!cpass.pass)
                continue;
            for (const auto& tex_ref : cpass.pass->textures)
            {
                const uint32_t res_idx = tex_ref.resource.index;
                if (res_idx >= resource_count)
                    continue;

                const auto& res_desc = graph.original_graph.resources[res_idx];
                if (res_desc.type != ERGResourceType::TEXTURE)
                    continue;
                if (!physical_resources.contains(res_idx))
                    continue;

                const auto& phys = physical_resources.at(res_idx);
                const auto& tex_desc = std::get<RGTextureDescription>(res_desc.desc);
                const uint32_t mip_count = (tex_desc.mip_levels > 0u) ? tex_desc.mip_levels : 1u;

                VkImageViewType view_type;
                switch (tex_desc.dimension)
                {
                    case lux::rdesc::ETextureDimension::TEX_2D:       view_type = VK_IMAGE_VIEW_TYPE_2D;       break;
                    case lux::rdesc::ETextureDimension::TEX_2D_ARRAY: view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
                    case lux::rdesc::ETextureDimension::TEX_3D:       view_type = VK_IMAGE_VIEW_TYPE_3D;       break;
                    case lux::rdesc::ETextureDimension::CUBE:         view_type = VK_IMAGE_VIEW_TYPE_CUBE;     break;
                    default:                                           view_type = VK_IMAGE_VIEW_TYPE_2D;       break;
                }

                // Aspect from FORMAT (P1#20): a depth texture sampled before its
                // attachment producer must still get the DEPTH aspect; stencil is added
                // only for attachment views (a two-aspect SAMPLED view violates 01976).
                VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                if (isDepthStencilFormat_shared(tex_desc.format))
                {
                    aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
                    if (hasStencilComponent_shared(tex_desc.format)
                        && tex_ref.role == lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
                }
                const uint32_t layer_count =
                    (tex_desc.dimension == lux::rdesc::ETextureDimension::TEX_2D_ARRAY)
                        ? tex_desc.array_layers : 1u;
                const VkFormat vk_format = convertTextureFormat(tex_desc.format);

                auto makeView = [&](VkImage image, uint32_t base_mip, uint32_t level_count) -> VkImageView
                {
                    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                    vi.image    = image;
                    vi.viewType = view_type;
                    vi.format   = vk_format;
                    vi.subresourceRange.aspectMask     = aspect;
                    vi.subresourceRange.baseMipLevel   = base_mip;
                    vi.subresourceRange.levelCount     = level_count;
                    vi.subresourceRange.baseArrayLayer = 0;
                    vi.subresourceRange.layerCount     = layer_count;
                    VkImageView v = VK_NULL_HANDLE;
                    vkCreateImageView(device, &vi, context_.instanceContext().allocator(), &v);
                    return v;
                };

                for (uint32_t frame = 0; frame < frames_in_flight; ++frame)
                {
                    if (record_context.per_frame_views[res_idx][frame] != VK_NULL_HANDLE)
                        continue;

                    VkImage image = reinterpret_cast<VkImage>(phys.getHandle(frame));
                    if (image == VK_NULL_HANDLE)
                        continue;

                    // FULL-mip view (whole chain): single-mip resources get a 1-level
                    // view exactly as before; mip_levels>1 resources get a chain view.
                    VkImageView full = makeView(image, 0u, mip_count);
                    if (full == VK_NULL_HANDLE)
                        return renderError<err::device::VulkanObjectCreationFailed>();
                    record_context.per_frame_views[res_idx][frame] = full;

                    // PER-MIP views (downsample src/dst, per-level sampling).
                    if (mip_count > 1u)
                    {
                        auto& by_mip = record_context.per_frame_views_by_mip[res_idx];
                        if (by_mip.size() < frames_in_flight)
                            by_mip.resize(frames_in_flight);
                        by_mip[frame].assign(mip_count, VK_NULL_HANDLE);
                        for (uint32_t m = 0; m < mip_count; ++m)
                        {
                            VkImageView mv = makeView(image, m, 1u);
                            if (mv == VK_NULL_HANDLE)
                                return renderError<err::device::VulkanObjectCreationFailed>();
                            by_mip[frame][m] = mv;
                        }
                    }
                }
            }
        }
        return std::nullopt;
    }

    void RGVulkanRecorder::destroyImageViews(RGRecordContext& record_context)
    {
        VkDevice device = context_.logicalDevice();

        for (auto& frame_vec : record_context.per_frame_views)
        {
            for (VkImageView v : frame_vec)
            {
                vkDestroyImageView(device, v, context_.instanceContext().allocator());
            }
        }
        record_context.per_frame_views.clear();

        for (auto& res_vec : record_context.per_frame_views_by_mip)
            for (auto& frame_vec : res_vec)
                for (VkImageView v : frame_vec)
                    vkDestroyImageView(device, v, context_.instanceContext().allocator());
        record_context.per_frame_views_by_mip.clear();

        for (VkImageView view : record_context.attachment_views)
        {
            vkDestroyImageView(device, view, context_.instanceContext().allocator());
        }
        record_context.attachment_views.clear();

        for (VkImageView view : record_context.extra_views)
        {
            vkDestroyImageView(device, view, context_.instanceContext().allocator());
        }
        record_context.extra_views.clear();
    }

    //(已删:RGVulkanRecorder::resolveImageView —— 全仓零调用、非虚,
    // 函数体内的三重保护随它一起是死的。热路径实际走
    // PassRecordContext::resolveTextureView,直接查 per_frame_views_ 表。)
}
