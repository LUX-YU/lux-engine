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
            std::uint32_t pass_index,
            RenderPassKey& out_key,
            RenderError& out_error)
        {
            RenderPassKey key{};
            uint32_t samples = 1;
            bool has_attachment = false;

            for (const auto& tex_ref : pass.textures)
            {
                const bool is_attachment =
                    tex_ref.role == lux::render::ETextureRole::COLOR_ATTACHMENT
                    || tex_ref.role == lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT;
                if (!is_attachment)
                    continue;

                has_attachment = true;

                const auto* tex_desc = find_texture_desc(graph, tex_ref.resource);
                if (!tex_desc)
                {
                    out_error = renderError<err::graph::AttachmentFormatUnknown>(
                        pass_index, tex_ref.resource.index);
                    return false;
                }

                // Record sample count (simple approach: take the first sample > 1)
                if (samples == 1 && tex_desc->sample > 1)
                    samples = tex_desc->sample;

                switch (tex_ref.role)
                {
                case lux::render::ETextureRole::COLOR_ATTACHMENT:
                {
                    VkFormat fmt = convertTextureFormat(tex_desc->format);
                    if (fmt == VK_FORMAT_UNDEFINED)
                    {
                        out_error = renderError<err::graph::AttachmentFormatUnknown>(
                            pass_index, tex_ref.resource.index);
                        return false;
                    }
                    key.push_color_format(fmt);
                    break;
                }
                case lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT:
                {
                    VkFormat fmt = convertTextureFormat(tex_desc->format);
                    if (fmt == VK_FORMAT_UNDEFINED)
                    {
                        out_error = renderError<err::graph::AttachmentFormatUnknown>(
                            pass_index, tex_ref.resource.index);
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
                // 图形 pass 一个附件都没声明 —— 它画不到任何地方。
                out_error = renderError<err::graph::AttachmentPlanInvalid>(pass_index, 0u);
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

        // Attachment refs of one pass, split by role (declaration order kept
        // for colors — it defines the fragment output location order).
        struct PassAttachmentRefs
        {
            std::vector<uint32_t> colors;              ///< COLOR_ATTACHMENT resource indices
            uint32_t              depth = std::numeric_limits<uint32_t>::max();
            /// INPUT_ATTACHMENT-role reads: (resource index, declared input index)
            std::vector<std::pair<uint32_t, uint32_t>> inputs;
        };

        PassAttachmentRefs collect_pass_refs(const RGPassDescription& pass)
        {
            PassAttachmentRefs out{};
            for (const auto& ref : pass.textures)
            {
                switch (ref.role)
                {
                case lux::render::ETextureRole::COLOR_ATTACHMENT:
                    out.colors.push_back(ref.resource.index);
                    break;
                case lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT:
                    if (out.depth == std::numeric_limits<uint32_t>::max())
                        out.depth = ref.resource.index;
                    break;
                case lux::render::ETextureRole::INPUT_ATTACHMENT:
                    out.inputs.emplace_back(ref.resource.index, ref.input_attachment_index);
                    break;
                default:
                    break;
                }
            }
            return out;
        }

        // ── local-read merge (line-B design P2) ─────────────────────────────
        // Try to absorb `pass` (whose key is incompatible with the group's)
        // into `group` as a local-read consumer: every INPUT_ATTACHMENT read
        // must target an attachment the group already owns, and the pass's own
        // outputs are APPENDED to the scope's attachment union. On success the
        // group key becomes the union and per-pass remaps are recorded; the
        // caller must not finalize the group. Inert until a feature declares
        // inputRead() (no caller does today).
        bool try_local_read_merge(const RGGraphDescription& graph,
                                  RGRenderPassGroup& group,
                                  uint32_t pass_idx,
                                  const RGPassDescription& pass,
                                  const RenderPassKey& pass_key,
                                  uint32_t color_budget)
        {
            const PassAttachmentRefs refs = collect_pass_refs(pass);
            if (refs.inputs.empty())
                return false;
            if (pass_key.samples != group.key.samples)
                return false;

            // Seed the union from the group's FIRST pass (the same refs the
            // emit path uses for plain groups), once.
            if (group.union_color_res.empty()
                && group.union_depth_res == std::numeric_limits<uint32_t>::max())
            {
                const auto& first_pass = graph.passes[group.passes.front().pass_index];
                const PassAttachmentRefs first_refs = collect_pass_refs(first_pass);
                group.union_color_res = first_refs.colors;
                group.union_depth_res = first_refs.depth;
            }

            auto union_color_slot_of = [&](uint32_t res_idx) -> uint32_t {
                for (uint32_t s = 0; s < group.union_color_res.size(); ++s)
                    if (group.union_color_res[s] == res_idx)
                        return s;
                return std::numeric_limits<uint32_t>::max();
            };

            // Every input read must target an attachment already in the scope.
            for (const auto& [res_idx, input_idx] : refs.inputs)
            {
                (void)input_idx;
                const bool is_scope_color =
                    union_color_slot_of(res_idx) != std::numeric_limits<uint32_t>::max();
                const bool is_scope_depth = res_idx == group.union_depth_res;
                if (!is_scope_color && !is_scope_depth)
                    return false;
            }

            // Depth: the consumer either declares no depth attachment or the
            // scope's own depth.
            if (refs.depth != std::numeric_limits<uint32_t>::max()
                && refs.depth != group.union_depth_res)
                return false;

            // Append the consumer's own color outputs (dedupe by resource).
            std::vector<uint32_t> appended;
            for (uint32_t res_idx : refs.colors)
            {
                if (union_color_slot_of(res_idx) != std::numeric_limits<uint32_t>::max())
                    continue;
                // Refusing the merge is the correct fallback: the passes stay
                // separate, which is always legal. Merging past the DEVICE's
                // limit would not be.
                if (group.union_color_res.size() + appended.size()
                    >= color_budget)
                    return false;
                appended.push_back(res_idx);
            }
            for (uint32_t res_idx : appended)
            {
                const auto* desc = find_texture_desc(graph, RGResourceHandle{res_idx});
                if (!desc)
                    return false;
                group.union_color_res.push_back(res_idx);
                group.key.push_color_format(convertTextureFormat(desc->format));
            }

            // Per-pass remaps. Producers already in the group get identity
            // over the union prefix they wrote (their own declaration order),
            // UNUSED elsewhere.
            const uint32_t scope_colors =
                static_cast<uint32_t>(group.union_color_res.size());
            for (auto& p : group.passes)
            {
                if (!p.color_locations.empty())
                    continue;   // already remapped by an earlier merge
                const PassAttachmentRefs prefs =
                    collect_pass_refs(graph.passes[p.pass_index]);
                p.color_locations.assign(scope_colors, VK_ATTACHMENT_UNUSED);
                for (uint32_t loc = 0; loc < prefs.colors.size(); ++loc)
                {
                    const uint32_t slot = union_color_slot_of(prefs.colors[loc]);
                    if (slot != std::numeric_limits<uint32_t>::max())
                        p.color_locations[slot] = loc;
                }
                p.input_indices.assign(scope_colors, VK_ATTACHMENT_UNUSED);
            }

            // The consumer pass itself.
            RGPassInRenderPass entry{};
            entry.pass_index    = pass_idx;
            entry.subpass_index = static_cast<uint32_t>(group.passes.size());
            entry.color_locations.assign(scope_colors, VK_ATTACHMENT_UNUSED);
            for (uint32_t loc = 0; loc < refs.colors.size(); ++loc)
            {
                const uint32_t slot = union_color_slot_of(refs.colors[loc]);
                if (slot != std::numeric_limits<uint32_t>::max())
                    entry.color_locations[slot] = loc;
            }
            entry.input_indices.assign(scope_colors, VK_ATTACHMENT_UNUSED);
            for (const auto& [res_idx, input_idx] : refs.inputs)
            {
                const uint32_t slot = union_color_slot_of(res_idx);
                if (slot != std::numeric_limits<uint32_t>::max())
                    entry.input_indices[slot] = input_idx;
                else if (res_idx == group.union_depth_res)
                    entry.depth_input_index = input_idx;
            }
            group.passes.push_back(std::move(entry));
            group.key.subpass_count++;
            group.local_read = true;
            return true;
        }

    } // anonymous namespace

    RGRenderPassLayoutInfo RenderPassPlanner::plan(const RGGraphDescription& graph,
                                                   const RGDependencyInfo&   dep,
                                                   uint32_t                  max_color_attachments)
    {
        // Two independent ceilings, and the SMALLER wins:
        //   - the key struct's array capacity (compile-time, costs memory);
        //   - the device's maxColorAttachments (runtime, costs correctness).
        // A caller that does not know the device passes 0 and gets the capacity
        // — the old behaviour, preserved for scene-less test graphs.
        const uint32_t color_budget =
            max_color_attachments == 0
                ? RenderPassKey::kMaxColorAttachments
                : std::min(max_color_attachments, RenderPassKey::kMaxColorAttachments);

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
            if (!build_render_pass_key(graph, pass, pass_idx, per_pass_key[pass_idx], info.error))
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
                // Start a new group (full reset: a previous local-read merge
                // leaves union/local_read state behind otherwise)
                current_group = RGRenderPassGroup{};
                current_group.key    = key;
                current_group.key.subpass_count = 1;
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
                else if (try_local_read_merge(graph, current_group, pass_idx,
                                              graph.passes[pass_idx], key, color_budget))
                {
                    // Absorbed as a local-read consumer: the group
                    // key is now the attachment UNION and stays open — a
                    // following pass may still merge against the union.
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

                    // Start a new group (full reset — see above)
                    current_group = RGRenderPassGroup{};
                    current_group.key    = key;
                    current_group.key.subpass_count = 1;
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

        // ── Local-read merge guarantee ───────────────────────
        // A pass that DECLARED input-attachment reads but did NOT get merged
        // (scheduling put a foreign pass between it and its producer) would
        // run with a merged-scope pipeline against a standalone scope —
        // guaranteed validation errors. Fail the plan loudly instead.
        for (uint32_t pass_idx = 0; pass_idx < pass_count; ++pass_idx)
        {
            if (!is_graphics[pass_idx])
                continue;
            const auto refs = collect_pass_refs(graph.passes[pass_idx]);
            if (refs.inputs.empty())
                continue;
            const uint32_t gi = info.pass_to_group[pass_idx];
            if (gi == std::numeric_limits<uint32_t>::max()
                || !info.groups[gi].local_read)
            {
                info.valid = false;
                // 声明了 input-attachment 读,却没被并进生产者的 rendering scope
                // —— 中间插进了一个与它无数据边的 pass。检查 local-read 粘合规则
                // 与 before() 的排序。
                info.error = renderError<err::graph::AttachmentPlanInvalid>(pass_idx, 1u);
                return info;
            }
        }

        return info;
    }

} // namespace lux::render
