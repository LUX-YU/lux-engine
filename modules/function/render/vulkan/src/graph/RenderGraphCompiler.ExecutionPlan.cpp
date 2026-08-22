#include <lux/engine/render/graph/RenderGraphCompiler.hpp>
#include <lux/engine/render/core/FrustumCuller.hpp>
#include <lux/engine/render/graph/RGLoadOpPolicy.hpp>
#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <lux/engine/render/graph/ProgramEmitter.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>                       // domain slot resolution (kEngineSetShapes)
#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp> // domain set instance (record-time collapsed binding)
#include <lux/engine/render/graph/RGBarrierUtils.hpp>
#include <lux/engine/render/graph/vk_type_converter.hpp>   // convertVkImageLayout (neutral DS layout)
#include <algorithm>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <span>

namespace lux::render
{
    namespace
    {
        // Scan a write-list for the first resource that has a downstream reader.
        // Returns {consumer_pass_index, resource_index} or {MAX, MAX} if none found.
        // The consumer lookup is only for error diagnostics, so it runs lazily.
        struct ConsumedWriteResult { uint32_t consumer_pass; uint32_t resource; };

        ConsumedWriteResult findFirstConsumedWrite(
            uint32_t                  producer_pass_index,
            std::span<const uint32_t> write_list,
            const std::vector<std::vector<uint32_t>>& readers_by_resource) noexcept
        {
            for (uint32_t ri : write_list)
            {
                if (ri >= readers_by_resource.size() || readers_by_resource[ri].empty())
                {
                    continue;
                }
                // Found a consumed resource — locate which pass reads it (for error message)
                for (uint32_t ci : readers_by_resource[ri])
                    if (ci != producer_pass_index)
                    {
                        return {ci, ri};
                    }
                return {std::numeric_limits<uint32_t>::max(), ri};
            }
            return {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()};
        }

        // ProgramEmitter is defined in ProgramEmitter.hpp (sinclude).

        // Emit a BeginRendering command with prebuilt header + per-attachment view patches.
        void emitBeginRendering(ProgramEmitter& e, uint32_t pi,
                                const RGCompiledPass& cpass,
                                const RGCompiledGraph& compiled)
        {
            const uint32_t group_idx = compiled.render_pass_layout.pass_to_group[pi];
            const auto& group = compiled.render_pass_layout.groups[group_idx];

            // wire structs shared with the decode side — see RGCompiledGraph.hpp.
            using ViewPatch = ExecutionProgram::BeginRenderingViewPatch;
            using Header    = ExecutionProgram::BeginRenderingHeader;
            static_assert(std::is_trivially_copyable_v<Header> &&
                          std::is_trivially_copyable_v<ViewPatch>);
            static_assert(RenderPassKey::kMaxColorAttachments <= 8,
                          "lr_* masks in the wire header are 8-bit");

            lux::cxx::SmallVector<ViewPatch, 9> patches;
            if (group.local_read)
            {
                // Local-read merged group: the scope's attachment
                // list is the UNION recorded by the planner — the first pass's
                // own refs would miss the consumer-only outputs (e.g. the
                // lighting target), so patch from the union tables instead.
                for (uint32_t slot = 0; slot < group.union_color_res.size(); ++slot)
                {
                    ViewPatch vp{};
                    vp.resource_index = group.union_color_res[slot];
                    vp.is_depth = 0;
                    vp.color_slot = static_cast<uint8_t>(slot);
                    patches.push_back(vp);
                }
                if (group.union_depth_res != std::numeric_limits<uint32_t>::max())
                {
                    ViewPatch vp{};
                    vp.resource_index = group.union_depth_res;
                    vp.is_depth = 1;
                    vp.color_slot = 0;
                    patches.push_back(vp);
                }
            }
            else
            {
            uint32_t color_slot_idx = 0;
            for (const auto& tex_ref : cpass.pass->textures) {
                if (tex_ref.role == lux::render::ETextureRole::COLOR_ATTACHMENT
                    && color_slot_idx < group.key.color_count)
                {
                    ViewPatch vp{};
                    vp.resource_index = tex_ref.resource.index;
                    vp.is_depth = 0;
                    vp.color_slot = static_cast<uint8_t>(color_slot_idx++);
                    patches.push_back(vp);
                }
                else if (tex_ref.role == lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                {
                    ViewPatch vp{};
                    vp.resource_index = tex_ref.resource.index;
                    vp.is_depth = 1;
                    vp.color_slot = 0;
                    patches.push_back(vp);
                }
            }
            }

            const uint16_t total_size = static_cast<uint16_t>(
                sizeof(Header) + patches.size() * sizeof(ViewPatch));

            Header hdr{};
            hdr.group_idx    = group_idx;
            hdr.color_count  = group.key.color_count;
            hdr.color_op     = group.color_load_op;
            hdr.depth_op     = group.depth_load_op;
            hdr.depth_format = group.key.depth_stencil_format;
            hdr.patch_count  = static_cast<uint8_t>(patches.size());
            if (group.local_read)
            {
                hdr.lr_flags = 0x1;
                hdr.lr_color_input_mask = group.lr_input_mask;
                if (group.lr_depth_input)
                {
                    hdr.lr_flags |= 0x2;
                }
                for (uint32_t s = 0; s < group.union_color_load_ops.size() && s < 8; ++s)
                    if (group.union_color_load_ops[s] == VK_ATTACHMENT_LOAD_OP_CLEAR)
                    {
                        hdr.lr_color_clear_mask |= static_cast<uint8_t>(1u << s);
                    }
                for (uint32_t s = 0; s < group.union_color_store_ops.size() && s < 8; ++s)
                    if (group.union_color_store_ops[s] == VK_ATTACHMENT_STORE_OP_DONT_CARE)
                    {
                        hdr.store_dontcare_mask |= static_cast<uint8_t>(1u << s);
                    }
                if (group.union_depth_store_op == VK_ATTACHMENT_STORE_OP_DONT_CARE)
                {
                    hdr.lr_flags |= 0x4;
                }
            }
            else if (group.passes.size() == 1)
            {
                // plain 单 pass 组:scope == pass,storeOp 直接取该 pass 的
                // 逐附件判定。多 pass plain 组的 scope 收尾语义属组末 pass,
                // 与首 pass 附件序的对应关系不保证 —— 保守全 STORE。
                const auto& gp = group.passes.front();
                for (uint32_t s = 0; s < gp.pass_color_store_ops.size() && s < 8; ++s)
                    if (gp.pass_color_store_ops[s] == VK_ATTACHMENT_STORE_OP_DONT_CARE)
                    {
                        hdr.store_dontcare_mask |= static_cast<uint8_t>(1u << s);
                    }
                if (gp.pass_depth_store_op == VK_ATTACHMENT_STORE_OP_DONT_CARE)
                {
                    hdr.lr_flags |= 0x4;
                }
            }

            const uint32_t offset = static_cast<uint32_t>(e.program.command_data.size());
            {
                const auto* bytes = reinterpret_cast<const std::byte*>(&hdr);
                e.program.command_data.insert(e.program.command_data.end(), bytes, bytes + sizeof(Header));
            }
            if (!patches.empty()) {
                const auto* bytes = reinterpret_cast<const std::byte*>(patches.data());
                e.program.command_data.insert(e.program.command_data.end(),
                                              bytes, bytes + patches.size() * sizeof(ViewPatch));
            }
            e.program.commands.push_back({
                ExecutionProgram::Command::EType::BeginRendering, offset, total_size});
        }
    } // namespace

    RenderGraphCompiler::LiveAccessIndex
    RenderGraphCompiler::buildLiveAccessIndex(const RGCompiledGraph &compiled)
    {
        LiveAccessIndex idx;
        const size_t res_count = compiled.original_graph.resources.size();
        idx.order_of.assign(compiled.compiled_passes.size(),
                            std::numeric_limits<uint32_t>::max());
        idx.last_access_order.assign(res_count, 0u);
        idx.readers.resize(res_count);
        idx.image_writers.resize(res_count);

        for (uint32_t o = 0; o < compiled.execution_order.size(); ++o)
        {
            const uint32_t pi = compiled.execution_order[o];
            idx.order_of[pi] = o;
            const auto& rs = compiled.compiled_passes[pi].resources;
            auto touch = [&](uint32_t ri) {
                if (ri < res_count)
                {
                    idx.last_access_order[ri] = std::max(idx.last_access_order[ri], o);
                }
            };
            for (uint32_t ri : rs.read_images)
            {
                touch(ri);
                if (ri < res_count)
                {
                    idx.readers[ri].push_back(pi);
                }
            }
            for (uint32_t ri : rs.read_buffers)
            {
                touch(ri);
                if (ri < res_count)
                {
                    idx.readers[ri].push_back(pi);
                }
            }
            for (uint32_t ri : rs.write_images)
            {
                touch(ri);
                if (ri < res_count)
                {
                    idx.image_writers[ri].push_back(pi);
                }
            }
            for (uint32_t ri : rs.write_buffers)
            {
                touch(ri);
            }
        }
        return idx;
    }

    // 9) Pre-compute per-group attachment resource indices and loadOps.
    //
    // For each render-pass group, record which logical resource index provides
    // the color and depth attachments.  Pre-compute VkAttachmentLoadOp: the
    // first group to write a resource in execution order gets CLEAR; every
    // subsequent group gets LOAD.
    //
    // This moves the per-frame O(groups×passes×textures) scan into a single
    // compile-time pass, and replaces the per-frame unordered_set tracking in
    // the recorder with a direct field read from RGRenderPassGroup.
    void RenderGraphCompiler::computeAttachmentOps(RGCompiledGraph& compiled,
                                                  const LiveAccessIndex& access)
    {
        auto& groups = compiled.render_pass_layout.groups;

        // Assign CLEAR to the first group that writes each resource,
        // LOAD to all subsequent groups. Indexed by resource index.
        const size_t res_count = compiled.original_graph.resources.size();
        std::vector<bool> cleared_color(res_count, false);
        std::vector<bool> cleared_depth(res_count, false);

        // ── storeOp 推导:LiveAccessIndex 的执行序最后访问位置 ───────────
        // storeOp=DONT_CARE 条件:TRANSIENT 资源且本 pass/scope 之后执行序
        // 上再无任何访问(读或写;组内后续 pass 的 LOAD 也是访问)。注意
        // 不能用 resource lifetimes 的 first/last——那是按 pass index 而非
        // 执行序统计的。
        const auto& order_of          = access.order_of;
        const auto& last_access_order = access.last_access_order;

        auto derive_store_op = [&](uint32_t res_idx, uint32_t after_order) -> VkAttachmentStoreOp
        {
            if (res_idx >= res_count)
            {
                return VK_ATTACHMENT_STORE_OP_STORE;
            }
            const auto& res = compiled.original_graph.resources[res_idx];
            if (res.lifetime != ERGResourceLifetime::TRANSIENT)
            {
                return VK_ATTACHMENT_STORE_OP_STORE;
            }
            return last_access_order[res_idx] <= after_order
                ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                : VK_ATTACHMENT_STORE_OP_STORE;
        };

        // Pre-mark imported resources that can actually preserve prior content,
        // so their first consumer gets LOAD_OP_LOAD instead of LOAD_OP_CLEAR.
        // For UNDEFINED initial_layout, first write must CLEAR.
        for (size_t i = 0; i < res_count; ++i)
        {
            const auto& res = compiled.original_graph.resources[i];
            if (res.lifetime == ERGResourceLifetime::IMPORTED && res.import_info &&
                shouldPreserveFirstWriteLoadOp(
                    res.import_info->preserve_content,
                    res.import_info->initial_layout))
            {
                cleared_color[i] = true;
                cleared_depth[i] = true;
            }
        }

        std::vector<bool> group_visited(groups.size(), false);
        for (uint32_t pass_idx : compiled.execution_order)
        {
            if (pass_idx >= compiled.render_pass_layout.pass_to_group.size())
            {
                continue;
            }
            const uint32_t group_idx = compiled.render_pass_layout.pass_to_group[pass_idx];
            if (group_idx == std::numeric_limits<uint32_t>::max())
            {
                continue;
            }
            if (group_visited[group_idx])
            {
                continue;
            }
            group_visited[group_idx] = true;

            auto& group = groups[group_idx];

            if (group.local_read && !group.union_color_res.empty())
            {
                // Local-read merged scope: per-slot first-writer bookkeeping
                // over the attachment UNION. Every union resource counts as
                // written by this group — otherwise the next group writing
                // e.g. the lighting target would be misjudged as its first
                // writer and CLEAR away the merged scope's output.
                group.union_color_load_ops.assign(group.union_color_res.size(),
                                                  VK_ATTACHMENT_LOAD_OP_LOAD);
                for (size_t s = 0; s < group.union_color_res.size(); ++s)
                {
                    const uint32_t cr = group.union_color_res[s];
                    if (cr >= res_count)
                    {
                        continue;
                    }
                    group.union_color_load_ops[s] =
                        cleared_color[cr] ? VK_ATTACHMENT_LOAD_OP_LOAD
                                          : VK_ATTACHMENT_LOAD_OP_CLEAR;
                    cleared_color[cr] = true;
                }
                group.color_load_op = group.union_color_load_ops[0];

                const uint32_t dr = group.union_depth_res;
                if (dr < res_count)
                {
                    group.depth_load_op = cleared_depth[dr]
                        ? VK_ATTACHMENT_LOAD_OP_LOAD
                        : VK_ATTACHMENT_LOAD_OP_CLEAR;
                    cleared_depth[dr] = true;
                }

                // storeOp:按合并作用域末序判定。G-buffer 三色的最后读者
                // 是 scope 内的 lighting → DONT_CARE(tiler 免写回);
                // 深度被 scope 后的 HzbBuild 读 → STORE。
                uint32_t scope_end_order = 0;
                for (const auto& gp : group.passes)
                    if (gp.pass_index < order_of.size() &&
                        order_of[gp.pass_index] != std::numeric_limits<uint32_t>::max())
                        scope_end_order = std::max(scope_end_order, order_of[gp.pass_index]);

                group.union_color_store_ops.assign(group.union_color_res.size(),
                                                   VK_ATTACHMENT_STORE_OP_STORE);
                for (size_t s = 0; s < group.union_color_res.size(); ++s)
                    group.union_color_store_ops[s] =
                        derive_store_op(group.union_color_res[s], scope_end_order);
                if (dr < res_count)
                {
                    group.union_depth_store_op = derive_store_op(dr, scope_end_order);
                }

                // input-attachment 槽位掩码:一次聚合供 emit/serial 消费。
                group.lr_input_mask  = 0;
                group.lr_depth_input = false;
                for (const auto& gp : group.passes)
                {
                    for (uint32_t s = 0; s < gp.input_indices.size()
                         && s < RenderPassKey::kMaxColorAttachments; ++s)
                        if (gp.input_indices[s] != VK_ATTACHMENT_UNUSED)
                        {
                            group.lr_input_mask |= static_cast<uint8_t>(1u << s);
                        }
                    if (gp.depth_input_index != VK_ATTACHMENT_UNUSED)
                    {
                        group.lr_depth_input = true;
                    }
                }
                continue;
            }

            // 组按格式 key 合并,成员可能写完全不同的资源(如高亮链的
            // Mask/BlurH/BlurV 各写一张 R8)。首写判定必须落到 (pass, 资源)
            // 粒度,否则组内后续 pass 会对自己首写的附件 LOAD 未定义内容。
            for (auto& gp : group.passes)
            {
                const auto& cpass = compiled.compiled_passes[gp.pass_index];
                gp.pass_color_load_ops.clear();
                gp.pass_depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
                gp.pass_color_store_ops.clear();
                gp.pass_depth_store_op = VK_ATTACHMENT_STORE_OP_STORE;
                if (!cpass.pass)
                {
                    continue;
                }
                const uint32_t self_order =
                    gp.pass_index < order_of.size() ? order_of[gp.pass_index]
                                                    : std::numeric_limits<uint32_t>::max();
                for (const auto& tr : cpass.pass->textures)
                {
                    if (tr.role == lux::render::ETextureRole::COLOR_ATTACHMENT)
                    {
                        const uint32_t cr = tr.resource.index;
                        VkAttachmentLoadOp op = VK_ATTACHMENT_LOAD_OP_LOAD;
                        if (cr < res_count)
                        {
                            op = cleared_color[cr] ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                   : VK_ATTACHMENT_LOAD_OP_CLEAR;
                            cleared_color[cr] = true;
                        }
                        gp.pass_color_load_ops.push_back(op);
                        gp.pass_color_store_ops.push_back(derive_store_op(cr, self_order));
                    }
                    else if (tr.role == lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                    {
                        const uint32_t dr = tr.resource.index;
                        if (dr < res_count)
                        {
                            gp.pass_depth_load_op = cleared_depth[dr]
                                ? VK_ATTACHMENT_LOAD_OP_LOAD
                                : VK_ATTACHMENT_LOAD_OP_CLEAR;
                            cleared_depth[dr] = true;
                        }
                        gp.pass_depth_store_op = derive_store_op(dr, self_order);
                    }
                }
            }

            // 组级 op 供 fast path 使用(整组只在首 pass Begin 一次,附件
            // 即首 pass 的附件),取首 pass 的判定结果。
            if (!group.passes.empty())
            {
                const auto& first = group.passes.front();
                if (!first.pass_color_load_ops.empty())
                {
                    group.color_load_op = first.pass_color_load_ops.front();
                }
                group.depth_load_op = first.pass_depth_load_op;
            }
        }

        // ── F13 守卫:pending_clear 机制的覆盖边界显式化 ─────────────────
        // 被跳过的条件 pass 若静态持有某资源的 CLEAR,义务由 serial-plain
        // 路径的 pending_clear 转移给下一个写者;lr 合并组的组级 Begin 不
        // 消费该机制——"条件 pass 持 CLEAR、后续写者在 lr 组"的形状会让
        // 该 union 槽 LOAD 到本帧从未清空的内容。编译期拒绝,不留运行期洞。
        // (fast path 情形不存在:图含条件 pass 时 fast path 整体关闭。)
        {
            const auto& p2g = compiled.render_pass_layout.pass_to_group;
            auto writer_in_lr_group_after = [&](uint32_t res, uint32_t after_order) {
                for (uint32_t w : access.image_writers[res])
                {
                    if (access.order_of[w] <= after_order)
                    {
                        continue;
                    }
                    const uint32_t wg = w < p2g.size() ? p2g[w]
                                                       : std::numeric_limits<uint32_t>::max();
                    if (wg != std::numeric_limits<uint32_t>::max() && groups[wg].local_read)
                    {
                        return true;
                    }
                }
                return false;
            };
            for (const auto& group : groups)
            {
                if (group.local_read)
                {
                    continue;
                }
                for (const auto& gp : group.passes)
                {
                    const auto& cpass = compiled.compiled_passes[gp.pass_index];
                    if (!cpass.condition || !cpass.pass)
                    {
                        continue;
                    }
                    const uint32_t self_order = access.order_of[gp.pass_index];
                    uint32_t decl = 0;
                    for (const auto& tr : cpass.pass->textures)
                    {
                        if (tr.role != lux::render::ETextureRole::COLOR_ATTACHMENT)
                        {
                            continue;
                        }
                        const uint32_t d = decl++;
                        if (d >= gp.pass_color_load_ops.size() ||
                            gp.pass_color_load_ops[d] != VK_ATTACHMENT_LOAD_OP_CLEAR)
                            continue;
                        if (tr.resource.index < res_count &&
                            writer_in_lr_group_after(tr.resource.index, self_order))
                        {
                            // 下一个写方落在 local-read 合并作用域里,pending-clear
                            // 的交接到不了合并的 Begin。改成让一个无条件 pass 拥有
                            // 这次 CLEAR。
                            compiled.compile_error = renderError<err::graph::ConditionalPassOwnsClear>(
                                cpass.pass_index, tr.resource.index);
                            return;
                        }
                    }
                }
            }
        }
    }

    // 10) Classify conditional passes as Elective.
    //
    // A pass with a runtime condition lambda is "Intra-Group Additive" when its
    // render-pass group also contains at least one unconditional pass.  This
    // guarantees that:
    //   • The group's CLEAR loadOp is always emitted by the unconditional pass,
    //     so skipping the elective pass never changes the first-writer.
    //   • Same-group passes share identical attachment layouts, so skipping
    //     does not break the pre-computed barrier chain.
    //
    // Isolated conditional passes (sole member of their group, or the unique
    // first-writer of a resource) currently do NOT exist in the engine.
    // If one is ever added, this function should assert/error rather than
    // silently producing an incorrect runtime schedule.
    bool RenderGraphCompiler::classifyElectivePasses(RGCompiledGraph& compiled,
                                                     const LiveAccessIndex& access)
    {
        const auto& groups = compiled.render_pass_layout.groups;
        const uint32_t pass_count = static_cast<uint32_t>(compiled.compiled_passes.size());
        const uint32_t resource_count = static_cast<uint32_t>(compiled.original_graph.resources.size());

        // 反查索引统一走 LiveAccessIndex(活 pass、执行序基准;F6/F12)。
        // image 写者用于链的读侧校验;buffer 读由 computeBarriers 的条件读
        // 累积规则兜底,无需链内约束。
        const auto& readers_by_resource       = access.readers;
        const auto& image_writers_by_resource = access.image_writers;

        // 条件链判定(condition_tag != 0):P 写的每个资源的每个读者要么是
        // P 自己,要么是同 tag 的条件 pass —— 链要么全跑要么全跳,跳过时
        // 链外没有人读到未写的内容。图外导出(如 Composite 写 SceneColor)
        // 没有图内读者,天然放行:跳过仅意味着少一层叠加。
        // 读侧:P 读的 image 必须链内产(写者全同 tag)——链外 image 读会
        // 让布局转换 barrier 落在可跳过的 pass 上,跳过即断链;buffer 无布
        // 局,读同步由 computeBarriers 的累积规则复制给后续读者,不设限。
        auto chain_classify = [&](const RGCompiledPass& cp) -> bool
        {
            const uint64_t tag = cp.pass ? cp.pass->condition_tag : 0u;
            if (tag == 0u)
            {
                return false;
            }
            auto readers_ok = [&](uint32_t ri)
            {
                for (uint32_t reader : readers_by_resource[ri])
                {
                    if (reader == cp.pass_index)
                    {
                        continue;
                    }
                    const auto& rp = compiled.compiled_passes[reader];
                    if (!rp.condition || !rp.pass || rp.pass->condition_tag != tag)
                    {
                        return false;
                    }
                }
                return true;
            };
            for (uint32_t ri : cp.resources.write_images)
            {
                if (ri < resource_count && !readers_ok(ri))
                {
                    return false;
                }
            }
            for (uint32_t ri : cp.resources.write_buffers)
            {
                if (ri < resource_count && !readers_ok(ri))
                {
                    return false;
                }
            }
            for (uint32_t ri : cp.resources.read_images)
            {
                if (ri >= resource_count)
                {
                    continue;
                }
                for (uint32_t writer : image_writers_by_resource[ri])
                {
                    if (writer == cp.pass_index)
                    {
                        continue;
                    }
                    const auto& wp = compiled.compiled_passes[writer];
                    if (!wp.condition || !wp.pass || wp.pass->condition_tag != tag)
                    {
                        return false;
                    }
                }
            }
            return true;
        };

        for (auto& cpass : compiled.compiled_passes)
        {
            if (!cpass.condition)
            {
                cpass.elective_kind = EElectiveKind::NONE;
                continue;
            }

            // Find the render-pass group this pass belongs to.
            const uint32_t pi = cpass.pass_index;
            const uint32_t group_idx = (pi < compiled.render_pass_layout.pass_to_group.size())
                                       ? compiled.render_pass_layout.pass_to_group[pi]
                                       : std::numeric_limits<uint32_t>::max();

            if (group_idx == std::numeric_limits<uint32_t>::max())
            {
                // Non-graphics (compute/transfer) conditional pass has no render-pass
                // group, so runtime skip is only safe when it has no downstream readers.
                auto result = findFirstConsumedWrite(cpass.pass_index,
                                  cpass.resources.write_images, readers_by_resource);
                if (result.resource == std::numeric_limits<uint32_t>::max())
                    result = findFirstConsumedWrite(cpass.pass_index,
                                  cpass.resources.write_buffers, readers_by_resource);

                if (result.resource != std::numeric_limits<uint32_t>::max())
                {
                    // 下游读者若全在同一条件链内(同 condition_tag),整链
                    // 原子跳过是安全的。
                    if (chain_classify(cpass))
                    {
                        cpass.elective_kind = EElectiveKind::CONDITIONAL_CHAIN;
                        continue;
                    }

                    // 写方是条件 pass,读方在条件链之外 —— 条件不成立时读到的是
                    // 未定义内容。给整条链一个 setCondition(cond, tag),或者去掉条件。
                    compiled.compile_error =
                        renderError<err::graph::ConditionalPassWritesUnconditionalRead>(
                            cpass.pass_index, result.resource);
                    return false;
                }

                cpass.elective_kind = EElectiveKind::INTRA_GROUP_ADDITIVE;
                continue;
            }

            const auto& group = groups[group_idx];
            bool has_unconditional_sibling = false;
            for (const auto& gp : group.passes)
            {
                const auto& sibling = compiled.compiled_passes[gp.pass_index];
                if (!sibling.condition)
                {
                    has_unconditional_sibling = true;
                    break;
                }
            }

            if (has_unconditional_sibling)
            {
                cpass.elective_kind = EElectiveKind::INTRA_GROUP_ADDITIVE;
            }
            else if (chain_classify(cpass))
            {
                // 全条件组:逐 pass 校验其写的资源只被同链读。serial 路径
                // 每 pass 独立 Begin + 逐资源 loadOp,全跳时零录制,组本身
                // 不需要无条件成员兜底。
                cpass.elective_kind = EElectiveKind::CONDITIONAL_CHAIN;
            }
            else
            {
                compiled.compile_error =
                    renderError<err::graph::AllConditionalPassGroup>(cpass.pass_index);
                return false;
            }
        }
        return true;
    }

    // =====================================================================
    //  Kernel-driven fast-path compiler stages
    // =====================================================================

    // ---------------------------
    // 11) canBuildFastPath
    // ---------------------------
    //
    // Returns true when every live pass is eligible for compiled execution.
    // RecorderFallback passes force mixed mode but no longer prevent the
    // compiler from building execution products for other passes.

    bool RenderGraphCompiler::canBuildFastPath(const RGCompiledGraph& compiled)
    {
        bool has_live_pass = false;
        for (const auto& cpass : compiled.compiled_passes)
        {
            if (cpass.pass == nullptr)
            {
                continue;
            }
            has_live_pass = true;
            if (cpass.execution_mode == EPassExecutionMode::RECORDER_FALLBACK)
            {
                return false;
            }
            // 条件 pass 不进 fast path:executeFast 线性重放整个 program,
            // 没有逐 pass 跳过能力。(条件 pass 依然携带 span——serial 混合
            // 路径按 cond_skip 决定是否重放。)
            if (cpass.condition)
            {
                return false;
            }
        }
        return has_live_pass;
    }

    // ---------------------------
    // 11.1) computeMeshBucketLayout
    // ---------------------------
    //
    // Scans all compiled MeshDraw passes and generates one MeshLane per
    // MDC entry.  Lanes are sorted by (pass_index, pipeline, geometry_kind,
    // bucket_id) to minimise runtime pipeline switches.  Buffer offsets
    // are assigned sequentially.

    void RenderGraphCompiler::computeMeshBucketLayout(RGCompiledGraph& compiled,
                                                      PipelineManager& pipeline_manager)
    {
        MeshBucketLayoutPlan plan;

        // Collect lanes: each kernel with a contribute_mesh hook adds its lanes.
        for (uint32_t pi = 0; pi < static_cast<uint32_t>(compiled.compiled_passes.size()); ++pi)
        {
            const auto& cpass = compiled.compiled_passes[pi];
            if (!cpass.pass)
            {
                continue;
            }

            const auto* desc = KernelRegistry::instance().find(cpass.pass->kernel_id);
            if (desc && desc->contribute_mesh)
            {
                desc->contribute_mesh(pi, cpass, plan, pipeline_manager);
            }
        }

        // Sort lanes by pass/pipeline and then IBO identity. Both pipeline and
        // index-buffer binds stay locally grouped without changing MDC offsets.
        std::sort(plan.lanes.begin(), plan.lanes.end(),
            [](const MeshLane& a, const MeshLane& b)
            {
                if (a.pass_index != b.pass_index)
                {
                    return a.pass_index < b.pass_index;
                }
                if (a.pipeline != b.pipeline)
                {
                    return a.pipeline < b.pipeline;
                }
                if (a.ibo_segment != b.ibo_segment)
                {
                    return a.ibo_segment < b.ibo_segment;
                }
                if (a.index_type != b.index_type)
                {
                    return a.index_type < b.index_type;
                }
                if (a.geometry_kind != b.geometry_kind)
                {
                    return a.geometry_kind < b.geometry_kind;
                }
                return a.bucket_id < b.bucket_id;
            }
        );

        // Assign offsets matching cull shader addressing.
        // Each lane maps to exactly one MDC entry.
        // indirect_offset = mdc_index * 20, count_offset = mdc_index * 4, maxDraw = 1
        VkPipeline prev_pipeline = VK_NULL_HANDLE;
        uint32_t   prev_pass     = ~0u;

        for (uint32_t i = 0; i < static_cast<uint32_t>(plan.lanes.size()); ++i)
        {
            auto& lane = plan.lanes[i];
            lane.lane_id         = i;
            lane.indirect_offset = static_cast<VkDeviceSize>(lane.mdc_index) * kIndirectCommandSize;
            lane.count_offset    = static_cast<VkDeviceSize>(lane.mdc_index) * sizeof(uint32_t);
            lane.bind_pipeline   = (lane.pipeline != prev_pipeline || lane.pass_index != prev_pass);
            prev_pipeline = lane.pipeline;
            prev_pass     = lane.pass_index;
        }

        // Size totals from max mdc_index
        uint32_t max_mdc = 0;
        for (const auto& lane : plan.lanes)
        {
            max_mdc = std::max(max_mdc, lane.mdc_index + 1);
        }
        plan.total_indirect_size = static_cast<VkDeviceSize>(max_mdc) * kIndirectCommandSize;
        plan.total_count_size    = static_cast<VkDeviceSize>(max_mdc) * sizeof(uint32_t);

        compiled.mesh_bucket_layout = std::move(plan);
    }

    // ---------------------------
    // 11.2) computeViewAllocatorPlan
    // ---------------------------
    //
    // Computes a per-view arena layout from the mesh bucket layout and
    // frustum data requirements.  Each region is aligned to a conservative
    // 256-byte boundary which satisfies all known Vulkan minimum offset
    // alignment requirements.

    void RenderGraphCompiler::computeViewAllocatorPlan(RGCompiledGraph& compiled)
    {
        constexpr VkDeviceSize kAlignment = 256; // Conservative min*OffsetAlignment

        auto alignUp = [](VkDeviceSize size, VkDeviceSize align) -> VkDeviceSize {
            return (size + align - 1) & ~(align - 1);
        };

        ViewAllocatorPlan plan;
        VkDeviceSize offset = 0;

        // Region 0: Mesh indirect buffer
        if (compiled.mesh_bucket_layout.has_value() && compiled.mesh_bucket_layout->total_indirect_size > 0)
        {
            ViewArenaRegion region{};
            region.region_id = static_cast<uint32_t>(plan.regions.size());
            region.offset    = offset;
            region.size      = compiled.mesh_bucket_layout->total_indirect_size;
            region.alignment = kAlignment;
            region.usage     = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            plan.regions.push_back(region);
            offset = alignUp(offset + region.size, kAlignment);
        }

        // Region 1: Mesh draw-count buffer
        if (compiled.mesh_bucket_layout.has_value() && compiled.mesh_bucket_layout->total_count_size > 0)
        {
            ViewArenaRegion region{};
            region.region_id = static_cast<uint32_t>(plan.regions.size());
            region.offset    = offset;
            region.size      = alignUp(compiled.mesh_bucket_layout->total_count_size, 4);
            region.alignment = kAlignment;
            region.usage     = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                             | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            plan.regions.push_back(region);
            offset = alignUp(offset + region.size, kAlignment);
        }

        // Region 2: conservative per-view cull-data arena. View cull records are
        // exact-origin ViewCullData (128B); shadow users need no more than this
        // per contributed slice, so one typed stride safely covers both.
        {
            constexpr VkDeviceSize kCullDataSize = kViewFrustumStrideBytes;

            // Accumulate arena requirements from all registered kernels.
            ViewArenaContribution arena_accum{};
            for (const auto& pass : compiled.original_graph.passes)
            {
                const auto* desc = KernelRegistry::instance().find(pass.kernel_id);
                if (desc && desc->contribute_arena)
                {
                    desc->contribute_arena(pass, arena_accum);
                }
            }

            const VkDeviceSize frustum_total =
                kCullDataSize * arena_accum.frustum_ubo_count
                + kCullDataSize * arena_accum.shadow_slice_count;
            if (frustum_total > 0)
            {
                ViewArenaRegion region{};
                region.region_id = static_cast<uint32_t>(plan.regions.size());
                region.offset    = offset;
                region.size      = frustum_total;
                region.alignment = kAlignment;
                region.usage     = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                plan.regions.push_back(region);
                offset = alignUp(offset + region.size, kAlignment);
            }
        }

        plan.total_arena_size = offset;
        plan.max_views = 4;   // Conservative default; can be tuned per renderer

        compiled.view_allocator_plan = std::move(plan);
    }

    // ---------------------------
    // 11.3) buildBarrierProgram
    // ---------------------------
    //
    // Converts the per-pass prebuilt barrier arrays into a BarrierProgram with
    // separate first-view and subsequent-view sequences.  For subsequent views,
    // the imported resource's oldLayout is patched using imported_final_states.

    void RenderGraphCompiler::buildBarrierProgram(RGCompiledGraph& compiled)
    {
        BarrierProgram program;
        lux::cxx::SmallVector<uint8_t, 64> subsequent_view_patched;
        if (!compiled.original_graph.resources.empty())
        {
            subsequent_view_patched.assign(compiled.original_graph.resources.size(), 0u);
        }

        for (uint32_t exec_idx = 0; exec_idx < static_cast<uint32_t>(compiled.execution_order.size()); ++exec_idx)
        {
            const uint32_t pi = compiled.execution_order[exec_idx];
            const auto& sync = compiled.compiled_passes[pi].sync;

            if (sync.prebuilt_image_barriers.empty() && sync.prebuilt_buffer_barriers.empty())
            {
                continue;
            }

            // First-view barriers: verbatim copy of prebuilt barriers
            {
                BarrierProgram::BarrierGroup group{};
                group.pass_index = pi;
                group.image_barriers.assign(sync.prebuilt_image_barriers.begin(),
                                            sync.prebuilt_image_barriers.end());
                group.buffer_barriers.assign(sync.prebuilt_buffer_barriers.begin(),
                                             sync.prebuilt_buffer_barriers.end());
                group.image_patch_resource_idx.assign(sync.image_patch_resource_idx.begin(),
                                                      sync.image_patch_resource_idx.end());
                group.buffer_patch_resource_idx.assign(sync.buffer_patch_resource_idx.begin(),
                                                       sync.buffer_patch_resource_idx.end());
                program.first_view_barriers.push_back(std::move(group));
            }

            // Subsequent-view barriers: patch the first touch per imported resource
            // to match the previous view's deterministic final state.
            {
                BarrierProgram::BarrierGroup group{};
                group.pass_index = pi;
                group.buffer_barriers.assign(sync.prebuilt_buffer_barriers.begin(),
                                             sync.prebuilt_buffer_barriers.end());
                group.buffer_patch_resource_idx.assign(sync.buffer_patch_resource_idx.begin(),
                                                       sync.buffer_patch_resource_idx.end());

                for (size_t bi = 0; bi < sync.prebuilt_image_barriers.size(); ++bi)
                {
                    VkImageMemoryBarrier2 barrier = sync.prebuilt_image_barriers[bi];
                    const uint32_t ri = sync.image_patch_resource_idx[bi];
                    uint8_t src_is_final = 0u;

                    const bool can_patch_first_touch = ri < subsequent_view_patched.size()
                                                    && !subsequent_view_patched[ri]
                                                    && ri < compiled.imported_final_state_lut.size();
                    if (can_patch_first_touch)
                    {
                        const uint32_t lut = compiled.imported_final_state_lut[ri];
                        if (lut != RGCompiledGraph::kInvalidSlotIdx &&
                            lut < compiled.imported_final_states.size())
                        {
                            barrier.srcStageMask = compiled.imported_final_states[lut].stage_mask;
                            barrier.srcAccessMask = compiled.imported_final_states[lut].access_mask;
                            barrier.oldLayout = compiled.imported_final_states[lut].layout;
                            subsequent_view_patched[ri] = 1u;
                            // 标记"src 来自上一录制的 final 状态"——录制器
                            // 按当前 target 的 final_layout 补丁这三个字段。
                            src_is_final = 1u;
                        }
                    }
                    group.image_barriers.push_back(barrier);
                    group.image_patch_resource_idx.push_back(ri);
                    group.image_src_is_final_state.push_back(src_is_final);
                }
                program.subsequent_view_barriers.push_back(std::move(group));
            }
        }

        // Final barriers
        if (!compiled.prebuilt_final_barriers.empty())
        {
            BarrierProgram::BarrierGroup final_group{};
            final_group.pass_index = ~0u;
            final_group.image_barriers.assign(compiled.prebuilt_final_barriers.begin(),
                                              compiled.prebuilt_final_barriers.end());
            final_group.image_patch_resource_idx.assign(compiled.final_patch_resource_idx.begin(),
                                                        compiled.final_patch_resource_idx.end());
            program.final_barriers.push_back(std::move(final_group));
        }

        // Build precomputed pass→barrier-group lookup tables so that
        // replayExecutionRange does not need to rebuild them each frame.
        {
            const uint32_t pass_count = static_cast<uint32_t>(compiled.compiled_passes.size());
            auto buildIndex = [&](const std::vector<BarrierProgram::BarrierGroup>& groups)
            {
                std::vector<uint32_t> idx(pass_count, RGCompiledGraph::kInvalidSlotIdx);
                for (uint32_t gi = 0; gi < static_cast<uint32_t>(groups.size()); ++gi)
                {
                    if (groups[gi].pass_index < pass_count)
                    {
                        idx[groups[gi].pass_index] = gi;
                    }
                }
                return idx;
            };
            program.first_view_group_by_pass = buildIndex(program.first_view_barriers);
            program.subsequent_view_group_by_pass = buildIndex(program.subsequent_view_barriers);
        }

        compiled.barrier_program = std::move(program);
    }

    // ---------------------------
    // 11.4) buildQueueSubmitProgram
    // ---------------------------
    //
    // Converts the per-queue execution orders and cross-queue dependencies
    // into a sequence of SubmissionTemplate entries.

    void RenderGraphCompiler::buildQueueSubmitProgram(RGCompiledGraph& compiled)
    {
        QueueSubmitProgram program;
        const auto& mqi = compiled.multi_queue_info;

        auto makeSubmission = [&](ERGQueueType queue, const std::vector<uint32_t>& order)
        {
            if (order.empty())
            {
                return;
            }

            QueueSubmitProgram::SubmissionTemplate tmpl{};
            tmpl.queue = queue;

            for (uint32_t pi : order)
            {
                const auto& cpass = compiled.compiled_passes[pi];
                for (const auto& dep : cpass.wait_dependencies)
                {
                    VkSemaphoreSubmitInfo wait_info{};
                    wait_info.sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                    wait_info.semaphore   = dep.semaphore;
                    wait_info.value       = dep.signal_value;
                    wait_info.stageMask   = dep.wait_stage;
                    tmpl.wait_templates.push_back(wait_info);
                }
                for (const auto& dep : cpass.signal_dependencies)
                {
                    VkSemaphoreSubmitInfo signal_info{};
                    signal_info.sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                    signal_info.semaphore   = dep.semaphore;
                    signal_info.value       = dep.signal_value;
                    signal_info.stageMask   = dep.signal_stage;
                    tmpl.signal_templates.push_back(signal_info);
                }
            }

            program.submissions.push_back(std::move(tmpl));
        };

        makeSubmission(ERGQueueType::COMPUTE,  mqi.compute_order);
        makeSubmission(ERGQueueType::TRANSFER, mqi.transfer_order);
        makeSubmission(ERGQueueType::GRAPHICS, mqi.graphics_order);

        compiled.queue_submit_program = std::move(program);
    }

    // ---------------------------
    // 11.5) buildExecutionProgram
    // ---------------------------
    //
    // Generates the bytecode-style ExecutionProgram from the compiled pass
    // sequence.  Each compiled pass is translated into a linear sequence of
    // Command entries with arguments stored in a flat data blob.

    void RenderGraphCompiler::buildExecutionProgram(RGCompiledGraph& compiled)
    {
        ExecutionProgram program;
        program.pass_spans.resize(compiled.compiled_passes.size());
        const bool full_fast_path = canBuildFastPath(compiled);

        ProgramEmitter emitter{program, compiled};

        for (uint32_t exec_idx = 0; exec_idx < static_cast<uint32_t>(compiled.execution_order.size()); ++exec_idx)
        {
            const uint32_t pi = compiled.execution_order[exec_idx];
            const auto& cpass = compiled.compiled_passes[pi];
            if (!cpass.pass)
            {
                continue;
            }
            if (cpass.execution_mode == EPassExecutionMode::RECORDER_FALLBACK)
            {
                continue;
            }

            const uint32_t span_start = static_cast<uint32_t>(program.commands.size());

            const bool is_graphics =
                cpass.pass->type == ERGPassType::GRAPHICS;

            // --- Set pass context (executor tracks current pass) ---
            emitter.emit(ExecutionProgram::Command::EType::SetPassContext,
                        &pi, sizeof(uint32_t));

            // --- Pre-pass barriers ---
            const auto& sync = cpass.sync;
            if (!sync.prebuilt_image_barriers.empty() || !sync.prebuilt_buffer_barriers.empty())
            {
                struct { uint32_t pass_index; uint32_t phase; } bd{pi, 0};
                emitter.emit(ExecutionProgram::Command::EType::PipelineBarrier,
                            &bd, static_cast<uint16_t>(sizeof(bd)));
            }

            // --- Begin rendering (with prebuilt template) ---
            if (cpass.render.begin_render_pass)
            {
                emitBeginRendering(emitter, pi, cpass, compiled);
            }

            // --- Local-read merged-group sub-pass boundary ---
            // Every graphics pass inside a local_read group sets its attachment
            // location / input-index remaps (command-buffer dynamic state); the
            // non-first passes additionally carry the by-region intra-scope
            // barrier. Emitted as its own EType so the dynamic-rendering
            // compatibility scan (no EType::PipelineBarrier inside a scope)
            // stays truthful.
            if (is_graphics)
            {
                const uint32_t gi = compiled.render_pass_layout.pass_to_group[pi];
                if (gi != std::numeric_limits<uint32_t>::max()
                    && compiled.render_pass_layout.groups[gi].local_read)
                {
                    const auto& group = compiled.render_pass_layout.groups[gi];
                    const RGPassInRenderPass* in_group = nullptr;
                    for (const auto& p : group.passes)
                        if (p.pass_index == pi) { in_group = &p; break; }
                    if (in_group)
                    {
                        // wire struct shared with the decode side (RGCompiledGraph.hpp).
                        ExecutionProgram::LocalReadBoundaryPayload b{};
                        b.color_count       = group.key.color_count;
                        b.emit_barrier      = in_group->subpass_index > 0 ? 1u : 0u;
                        b.depth_input_index = in_group->depth_input_index;
                        for (uint32_t s = 0; s < RenderPassKey::kMaxColorAttachments; ++s)
                        {
                            b.locations[s] = s < in_group->color_locations.size()
                                ? in_group->color_locations[s] : VK_ATTACHMENT_UNUSED;
                            b.input_indices[s] = s < in_group->input_indices.size()
                                ? in_group->input_indices[s] : VK_ATTACHMENT_UNUSED;
                        }
                        emitter.emit(ExecutionProgram::Command::EType::LocalReadBoundary,
                                     &b, static_cast<uint16_t>(sizeof(b)));
                    }
                }
            }

            // --- Bind pipeline ---
            const bool emit_pass_pipeline = full_fast_path
                ? cpass.render.bind_pipeline
                : (cpass.render.pipeline != VK_NULL_HANDLE);
            if (emit_pass_pipeline && cpass.render.pipeline != VK_NULL_HANDLE)
            {
                struct { VkPipeline pipeline; VkPipelineLayout layout; }
                    bp{cpass.render.pipeline, cpass.render.pipeline_layout};
                emitter.emit(ExecutionProgram::Command::EType::BindPipeline,
                            &bp, static_cast<uint16_t>(sizeof(bp)));
            }

            // --- Bind descriptor sets ---
            for (const auto& recipe : cpass.render.ds_bind_recipe)
            {
                struct { uint32_t slot; VkDescriptorSet set; }
                    bd{recipe.slot, recipe.immutable_set};
                const uint32_t cmd_idx = emitter.emit(
                    ExecutionProgram::Command::EType::BindDescriptorSets,
                    &bd, static_cast<uint16_t>(sizeof(bd)));

                if (recipe.resolve != DSBindRecipe::resolveImmutable)
                {
                    ExecutionProgram::DynamicPatch patch{};
                    patch.command_index     = cmd_idx;
                    patch.data_field_offset = static_cast<uint16_t>(sizeof(uint32_t)); // offset of 'set' field
                    patch.source            = ExecutionProgram::DynamicPatch::ESource::FrameIndex;
                    patch.source_param      = recipe.slot;
                    program.patches.push_back(patch);
                }
            }

            // --- Push constants (graphics: scene_index + view_index) ---
            if (is_graphics && !cpass.render.ds_bind_recipe.empty()
                && cpass.render.pipeline_layout != VK_NULL_HANDLE)
            {
                emitter.emit(ExecutionProgram::Command::EType::PushConstants,
                            &pi, sizeof(uint32_t));
            }

            // --- Viewport / Scissor (every graphics pass) ---
            if (is_graphics && !cpass.pass->manual_viewport)
            {
                emitter.emit(ExecutionProgram::Command::EType::SetViewport,
                            &pi, sizeof(uint32_t));
                emitter.emit(ExecutionProgram::Command::EType::SetScissor,
                            &pi, sizeof(uint32_t));
            }

            // --- Kernel-specific commands ---
            const uint32_t kernel_cmd_start = static_cast<uint32_t>(program.commands.size());
            if (cpass.execution_mode == EPassExecutionMode::COMPILED_CALLBACK)
            {
                emitter.emit(ExecutionProgram::Command::EType::InvokeKernelFn, nullptr, 0);
            }
            else
            {
                // Registry dispatch — O(1) lookup, zero coupling to specific kernels.
                const auto* desc = KernelRegistry::instance().find(cpass.pass->kernel_id);
                if (desc && desc->emit)
                {
                    desc->emit(emitter, pi, cpass, compiled);
                }
            }

            // Safety net: if a pass is classified as native but no kernel body command
            // was emitted, fall back to callback invocation when available.
            if (cpass.execution_mode == EPassExecutionMode::COMPILED_NATIVE
                && cpass.pass->kernel_fn
                && static_cast<uint32_t>(program.commands.size()) == kernel_cmd_start)
            {
                emitter.emit(ExecutionProgram::Command::EType::InvokeKernelFn, nullptr, 0);
            }

            // --- End rendering ---
            if (cpass.render.end_render_pass)
            {
                emitter.emit(ExecutionProgram::Command::EType::EndRendering, nullptr, 0);
            }

            auto& span = program.pass_spans[pi];
            span.first_command = span_start;
            span.one_past_last_command = static_cast<uint32_t>(program.commands.size());
            span.valid = span.one_past_last_command > span_start;
        }

        program.full_fast_path = full_fast_path;

        // Verify dynamic-rendering compatibility: no PipelineBarrier inside
        // a [BeginRendering, EndRendering) scope.
        {
            bool inside_scope = false;
            bool compatible   = true;
            for (const auto& cmd : program.commands)
            {
                if (cmd.type == ExecutionProgram::Command::EType::BeginRendering)
                {
                    inside_scope = true;
                }
                else if (cmd.type == ExecutionProgram::Command::EType::EndRendering)
                {
                    inside_scope = false;
                }
                else if (cmd.type == ExecutionProgram::Command::EType::PipelineBarrier && inside_scope)
                {
                    compatible = false;
                    break;
                }
            }
            program.dynamic_rendering_compatible = compatible;
        }

        compiled.execution_program = std::move(program);
    }


} // namespace lux::render
