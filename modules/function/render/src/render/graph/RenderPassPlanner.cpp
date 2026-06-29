#include <lux/engine/render/graph/RenderPassPlanner.hpp>
#include <lux/engine/render/graph/vk_type_converter.hpp>

namespace lux::render
{
    namespace
    {
        // Find texture description in the graph by handle, returns nullptr on failure
        const RGTextureDescription* find_texture_desc(const RGGraphDescription& graph, RGResourceHandle handle)
        {
            if (handle.index >= graph.resources.size())
                return nullptr;

            const auto& res = graph.resources[handle.index];
            if (res.type != ERGResourceType::TEXTURE)
                return nullptr;

            const auto* tex_desc = std::get_if<RGTextureDescription>(&res.desc);
            return tex_desc;
        }

        // Build a RenderPassKey for a graphics pass.
        // Returns false when attachment declarations are invalid.
        bool build_render_pass_key(
            const RGGraphDescription& graph,
            const RGPassDescription& pass,
            RenderPassKey& out_key,
            std::string& error_message)
        {
            RenderPassKey key{};
            uint32_t samples = 1;
            bool has_attachment = false;

            for (const auto& tex_ref : pass.textures)
            {
                const bool is_attachment =
                    tex_ref.role == lux::common::ETextureRole::COLOR_ATTACHMENT
                    || tex_ref.role == lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT;
                if (!is_attachment)
                    continue;

                has_attachment = true;

                const auto* tex_desc = find_texture_desc(graph, tex_ref.resource);
                if (!tex_desc)
                {
                    error_message = "RenderPassPlanner: pass '" + pass.name
                        + "' references an invalid attachment resource.";
                    return false;
                }

                // Record sample count (simple approach: take the first sample > 1)
                if (samples == 1 && tex_desc->sample > 1)
                    samples = tex_desc->sample;

                switch (tex_ref.role)
                {
                case lux::common::ETextureRole::COLOR_ATTACHMENT:
                {
                    VkFormat fmt = convertTextureFormat(tex_desc->format);
                    if (fmt == VK_FORMAT_UNDEFINED)
                    {
                        error_message = "RenderPassPlanner: pass '" + pass.name
                            + "' has COLOR_ATTACHMENT with undefined VkFormat.";
                        return false;
                    }
                    key.push_color_format(fmt);
                    break;
                }
                case lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT:
                {
                    VkFormat fmt = convertTextureFormat(tex_desc->format);
                    if (fmt == VK_FORMAT_UNDEFINED)
                    {
                        error_message = "RenderPassPlanner: pass '" + pass.name
                            + "' has DEPTH_STENCIL_ATTACHMENT with undefined VkFormat.";
                        return false;
                    }
                    // For simplicity, only take the first depth/stencil
                    if (key.depth_stencil_format == VK_FORMAT_UNDEFINED)
                        key.depth_stencil_format = fmt;
                    break;
                }
                default:
                    // SAMPLED / UNORDERED_ACCESS do not participate in render pass attachments
                    break;
                }
            }

            if (!has_attachment)
            {
                error_message = "RenderPassPlanner: graphics pass '" + pass.name
                    + "' has no color/depth attachments.";
                return false;
            }

            key.samples = samples;
            out_key = key;
            return true;
        }

        // Check whether two keys can share a single physical render pass
        bool is_render_pass_key_compatible(const RenderPassKey& a,
                                           const RenderPassKey& b)
        {
            if (a.samples != b.samples)
                return false;

            if (a.color_count != b.color_count)
                return false;

            for (uint32_t i = 0; i < a.color_count; ++i)
            {
                if (a.color_formats[i] != b.color_formats[i])
                    return false;
            }

            if (a.depth_stencil_format != b.depth_stencil_format)
                return false;

            return true;
        }

    } // anonymous namespace

    RGRenderPassLayoutInfo RenderPassPlanner::plan(const RGGraphDescription& graph, const RGDependencyInfo& dep)
    {
        RGRenderPassLayoutInfo info{};

        const uint32_t pass_count =
            static_cast<uint32_t>(graph.passes.size());

        info.pass_to_group.assign(pass_count, std::numeric_limits<uint32_t>::max());
        info.pass_to_subpass.assign(pass_count, 0u);

        // Pre-compute keys for all graphics passes
        std::vector<bool>        is_graphics(pass_count, false);
        std::vector<RenderPassKey> per_pass_key(pass_count);

        for (uint32_t pass_idx = 0; pass_idx < pass_count; ++pass_idx)
        {
            const auto& pass = graph.passes[pass_idx];
            if (pass.type != ERGPassType::GRAPHICS)
                continue;

            is_graphics[pass_idx]   = true;
            if (!build_render_pass_key(graph, pass, per_pass_key[pass_idx], info.error_message))
            {
                info.valid = false;
                return info;
            }
        }

        const auto& order = dep.pass_topological_order;

        // Scan graphics passes in topological order, group consecutive ones with compatible keys
        RGRenderPassGroup current_group{};
        bool has_current_group = false;

        for (uint32_t i = 0; i < order.size(); ++i)
        {
            const uint32_t pass_idx = order[i];

            if (!is_graphics[pass_idx])
            {
                // Non-graphics pass (compute/transfer) sits between graphics passes.
                // Must finalize the current group to prevent merging graphics passes
                // across an interleaved non-graphics pass (data hazard).
                if (has_current_group)
                {
                    const uint32_t group_index = static_cast<uint32_t>(info.groups.size());
                    info.groups.push_back(current_group);

                    for (const auto& p : current_group.passes)
                    {
                        info.pass_to_group[p.pass_index]   = group_index;
                        info.pass_to_subpass[p.pass_index] = p.subpass_index;
                    }

                    has_current_group = false;
                }
                continue;
            }

            const RenderPassKey& key = per_pass_key[pass_idx];

            if (!has_current_group)
            {
                // Start a new group
                current_group.key    = key;
                current_group.key.subpass_count = 1;
                current_group.passes.clear();
                current_group.passes.push_back(RGPassInRenderPass{
                    pass_idx,
                    /*subpass_index*/ 0u
                });

                has_current_group = true;
            }
            else
            {
                // Check if it can be merged into the current group
                if (is_render_pass_key_compatible(current_group.key, key))
                {
                    uint32_t subpass_idx = static_cast<uint32_t>(current_group.passes.size());
                    current_group.passes.push_back(RGPassInRenderPass{
                        pass_idx,
                        subpass_idx
                    });
                    current_group.key.subpass_count++;
                }
                else
                {
                    // Finalize the current group into info
                    const uint32_t group_index = static_cast<uint32_t>(info.groups.size());
                    info.groups.push_back(current_group);

                    // Fill mapping: pass -> group/subpass
                    for (const auto& p : current_group.passes)
                    {
                        info.pass_to_group[p.pass_index]   = group_index;
                        info.pass_to_subpass[p.pass_index] = p.subpass_index;
                    }

                    // Start a new group
                    current_group.key    = key;
                    current_group.key.subpass_count = 1;
                    current_group.passes.clear();
                    current_group.passes.push_back(RGPassInRenderPass{
                        pass_idx,
                        /*subpass_index*/ 0u
                    });
                }
            }
        }

        // Finalize the last group
        if (has_current_group)
        {
            const uint32_t group_index =
                static_cast<uint32_t>(info.groups.size());
            info.groups.push_back(current_group);

            for (const auto& p : current_group.passes)
            {
                info.pass_to_group[p.pass_index]   = group_index;
                info.pass_to_subpass[p.pass_index] = p.subpass_index;
            }
        }

        return info;
    }

} // namespace lux::render
