    void RGVulkanRecorder::record(RGResourceState& state, const RGCompiledGraph& compiled_graph, const RGFrameContext& frame_ctx, VkCommandBuffer target_cmd, ResourceRegistry* gpu_mgr)
    {
        auto& record_context = state.record_ctx;
        record_context.physical_resources_ptr = &state.physical_resources;
        // 运行时布局参数化的两个输入随录制下发(final barrier newLayout /
        // 后续帧首触 oldLayout 按当前 target 的 final_layout 补丁)。
        record_context.target_layout     = frame_ctx.target_layout;
        record_context.slot_resource_idx = compiled_graph.slot_resource_idx.data();
        // Inject per-slot image/view overrides into the per-frame override tables.
        // The compiled graph remains immutable; barrier and rendering code consults
        // per_frame_images / per_frame_views before falling back to physical_resources.
        for (size_t si = 0; si < kTargetSlotCount; ++si)
        {
            const uint32_t ri = compiled_graph.slot_resource_idx[si];
            if (ri == RGCompiledGraph::kInvalidSlotIdx) continue;

            const auto& binding = frame_ctx.imported_slots[si];
            if (binding.image == VK_NULL_HANDLE) continue;

            if (ri < record_context.per_frame_images.size() &&
                frame_ctx.frame_index < record_context.per_frame_images[ri].size())
                record_context.per_frame_images[ri][frame_ctx.frame_index] = binding.image;

            if (ri < record_context.per_frame_views.size() &&
                frame_ctx.frame_index < record_context.per_frame_views[ri].size())
                record_context.per_frame_views[ri][frame_ctx.frame_index] = binding.view;
        }

        updateImportedTransientDescriptors(
            context_.logicalDevice(),
            compiled_graph,
            record_context,
            frame_ctx.frame_index
        );

        // Reset per-frame binding state so stale handles from the previous frame
        // are not mistakenly treated as "already bound" during this frame's record().
        if (frame_ctx.frame_index < record_context.binding_states.size())
            record_context.binding_states[frame_ctx.frame_index].reset();

    #if !defined(NDEBUG)
        debug_named_pipelines_.clear();
    #endif

        VkCommandBuffer cmd = target_cmd;

        if (timestampRangeValid(
                record_context,
                compiled_graph,
                frame_ctx.frame_index
            ))
        {
            const uint32_t pass_count = static_cast<uint32_t>(
                compiled_graph.compiled_passes.size()
            );
            const uint32_t query_count = pass_count * 2u;
            const uint32_t query_base = frame_ctx.frame_index
                * record_context.timestamp_pass_capacity * 2u;
            const uint64_t retired_frame =
                record_context.timestamp_frame_ids[frame_ctx.frame_index];

            if (retired_frame != 0u)
            {
                auto& result = record_context.timestamp_result_scratch;
                result.assign(static_cast<std::size_t>(query_count) * 2u, 0u);
                const VkResult query_result = vkGetQueryPoolResults(
                    context_.logicalDevice(),
                    record_context.timestamp_pool,
                    query_base,
                    query_count,
                    result.size() * sizeof(uint64_t),
                    result.data(),
                    sizeof(uint64_t) * 2u,
                    VK_QUERY_RESULT_64_BIT
                        | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
                );

                auto snapshot = std::make_shared<RGGpuTimingSnapshot>();
                snapshot->frame_id = retired_frame;
                snapshot->passes.reserve(pass_count);
                // WITH_AVAILABILITY writes an availability value for every
                // requested timestamp even when the aggregate call returns
                // VK_NOT_READY.  Conditional/elective passes legitimately
                // leave their pair unavailable; do not discard all completed
                // pass timings because one such pass did not execute.  No WAIT
                // bit is used, so adoption remains non-blocking.
                if (query_result == VK_SUCCESS || query_result == VK_NOT_READY)
                {
                    const double tick_ms = static_cast<double>(
                        record_context.timestamp_period_nanoseconds
                    ) / 1'000'000.0;
                    for (uint32_t pass = 0; pass < pass_count; ++pass)
                    {
                        const std::size_t begin = pass * 4u;
                        const std::size_t end = begin + 2u;
                        if (result[begin + 1u] == 0u
                            || result[end + 1u] == 0u
                            || result[end] < result[begin])
                        {
                            continue;
                        }

                        const double milliseconds = static_cast<double>(
                            result[end] - result[begin]
                        ) * tick_ms;
                        const auto* description =
                            compiled_graph.compiled_passes[pass].pass;
                        snapshot->passes.push_back(RGGpuPassTiming{
                            description ? description->name : std::string{},
                            milliseconds,
                        });
                        snapshot->total_milliseconds += milliseconds;
                    }
                    snapshot->available = !snapshot->passes.empty();
                }
                record_context.latest_gpu_timing = snapshot;
                record_context.gpu_timing_history.push_back(
                    std::move(snapshot));
                constexpr std::size_t kGpuTimingHistoryCapacity = 4096u;
                if (record_context.gpu_timing_history.size() >
                    kGpuTimingHistoryCapacity)
                {
                    record_context.gpu_timing_history.pop_front();
                }
            }

            vkCmdResetQueryPool(
                cmd,
                record_context.timestamp_pool,
                query_base,
                query_count
            );
            record_context.timestamp_frame_ids[frame_ctx.frame_index] =
                frame_ctx.frame_id;
        }

        // ================================================================
        // A-01: Multi-queue command buffer setup
        // ================================================================
        const bool is_multi_queue = compiled_graph.multi_queue_info.has_async_work;
        VkCommandBuffer compute_cmd  = VK_NULL_HANDLE;
        VkCommandBuffer transfer_cmd = VK_NULL_HANDLE;

        if (is_multi_queue)
        {
            const VkCommandBufferBeginInfo begin_info{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr };

            if (frame_ctx.frame_index < record_context.compute_cmd_bufs.size())
            {
                compute_cmd = record_context.compute_cmd_bufs[frame_ctx.frame_index];
                vkResetCommandBuffer(compute_cmd, 0);
                vkBeginCommandBuffer(compute_cmd, &begin_info);
            }
            if (frame_ctx.frame_index < record_context.transfer_cmd_bufs.size())
            {
                transfer_cmd = record_context.transfer_cmd_bufs[frame_ctx.frame_index];
                vkResetCommandBuffer(transfer_cmd, 0);
                vkBeginCommandBuffer(transfer_cmd, &begin_info);
            }
        }

        // ================================================================
        // Path selection: fast path (ExecutionProgram) vs slow path (recorder lambdas)
        // ================================================================
        // Dynamic rendering forbids barriers inside an active rendering scope.
        // The compiler verifies that no PipelineBarrier falls between
        // BeginRendering / EndRendering and sets dynamic_rendering_compatible
        // accordingly, allowing the fast path when safe.
        const bool allow_full_fast =
            compiled_graph.hasFullFastPath()
            && (!record_context.use_dynamic_rendering
                || compiled_graph.execution_program->dynamic_rendering_compatible);
        if (allow_full_fast)
        {
            executeFast(cmd, compiled_graph, record_context, frame_ctx,
                        state.physical_resources, gpu_mgr);

            // Finalize async command buffers (fast path uses primary cmd only for now).
            buildMultiQueueSubmit(record_context, compiled_graph, cmd, compute_cmd, transfer_cmd);
        }
        else
        {

        // Marks passes that have been processed, prevents re-processing Subpasses within a Group when iterating execution_order
        // Reuse scratch buffer from record_context to avoid per-frame heap allocation
        auto& processed_passes = record_context.processed_passes_scratch;
        processed_passes.assign(compiled_graph.compiled_passes.size(), 0);

        // 条件快照:一帧一判(F14)。此后所有分支只读 cond_skip。
        auto& cond_skip = record_context.cond_skip_scratch;
        cond_skip.assign(compiled_graph.compiled_passes.size(), 0);
        for (uint32_t pi : compiled_graph.execution_order)
        {
            const auto& cp = compiled_graph.compiled_passes[pi];
            if (cp.condition && !(*cp.condition)())
                cond_skip[pi] = 1;
        }

        ExecutionReplayState mixed_replay_state{};
        if (compiled_graph.hasFastPath())
            mixed_replay_state.binding_state = &record_context.binding_states[frame_ctx.frame_index];

        auto compiled_span_for_pass = [&](uint32_t pass_index)
            -> const ExecutionProgram::PassCommandSpan*
        {
            if (!compiled_graph.execution_program.has_value())
                return nullptr;
            const auto& spans = compiled_graph.execution_program->pass_spans;
            if (pass_index >= spans.size())
                return nullptr;
            return spans[pass_index].valid ? &spans[pass_index] : nullptr;
        };

        const auto& physical_resources = state.physical_resources;

        // Helper lambda: submit pre-computed compile-time barriers for a pass.
        // A-01: accepts explicit VkCommandBuffer to support per-queue barrier submission.

        // ================================================================
        // Cross-view barrier fixup (multi-view same-scene recording)
        // ================================================================
        // When cross_view_index > 0 the same compiled graph is being recorded
        // again into the same command buffer.  Imported resources are NOT in
        // their declared initial state — they are in the deterministic final
        // state left by the previous view's recording.  We track which
        // imported resources have had their first-touch barrier patched so
        // each is fixed up exactly once.
        const PreBarrierGroupSelection pre_bsel = selectPreBarrierGroups(compiled_graph, frame_ctx);

        auto submit_barriers = [&](VkCommandBuffer barrier_cmd, uint32_t pass_index, const RGCompiledPass& cpass) {
            emitPassBarriers(barrier_cmd, pass_index, cpass,
                             pre_bsel.groups, pre_bsel.group_index_by_pass,
                             record_context, physical_resources, frame_ctx.frame_index);
        };

        // Per-graph execution order dump removed (was [RGDbg])

        // 被跳过的条件 pass 若静态持有某资源的 CLEAR(首写者身份),登记
        // 在此,由本帧内该资源的下一个写者把自己的 LOAD 升级为 CLEAR——
        // 否则它会 LOAD 到本帧从未清空的内容。常态(无跳过)下恒为空。
        std::vector<uint32_t> pending_clear_resources;

        // Iterate execution order
        for (uint32_t pass_idx : compiled_graph.execution_order)
        {
            // If this Pass has already been processed (as part of a Group), skip it
            if (processed_passes[pass_idx]) continue;

            const RGCompiledPass& cpass = compiled_graph.compiled_passes[pass_idx];

            // Conditional pass execution check (runtime lambda only; feature
            // enable/disable is handled at compile time via graph invalidation).
            if (cond_skip[pass_idx]) {
                processed_passes[pass_idx] = 1;
                continue;
            }

            // ===========================
            // Case 1: Compute / Async Compute Pass
            // ===========================
            if (cpass.pass->type == ERGPassType::COMPUTE || cpass.pass->type == ERGPassType::ASYNC_COMPUTE)
            {
                // A-01: Route to the correct command buffer based on queue assignment
                VkCommandBuffer pass_cmd = cmd;
                if (cpass.queue_type == ERGQueueType::COMPUTE && compute_cmd != VK_NULL_HANDLE)
                    pass_cmd = compute_cmd;

                submit_barriers(pass_cmd, pass_idx, cpass);
                if (const auto* span = compiled_span_for_pass(pass_idx))
                {
                    mixed_replay_state.current_pass = UINT32_MAX;
                    mixed_replay_state.current_layout = VK_NULL_HANDLE;
                    replayExecutionRange(pass_cmd, compiled_graph, record_context, frame_ctx,
                                         state.physical_resources, gpu_mgr,
                                         mixed_replay_state,
                                         span->first_command,
                                         span->one_past_last_command,
                                         false,
                                         true);
                }
                else
                {
                    writePassTimestamp(
                        pass_cmd,
                        record_context,
                        compiled_graph,
                        frame_ctx.frame_index,
                        pass_idx,
                        true
                    );
                    recordPassContent(pass_cmd, cpass, compiled_graph, record_context, frame_ctx,
                                      gpu_mgr, VK_PIPELINE_BIND_POINT_COMPUTE, {});
                    writePassTimestamp(
                        pass_cmd,
                        record_context,
                        compiled_graph,
                        frame_ctx.frame_index,
                        pass_idx,
                        false
                    );
                }
                processed_passes[pass_idx] = 1;
            }
            // ===========================
            // Case 1b: Transfer / Async Transfer Pass (A-01)
            // ===========================
            else if (cpass.pass->type == ERGPassType::TRANSFER || cpass.pass->type == ERGPassType::ASYNC_TRANSFER)
            {
                VkCommandBuffer pass_cmd = cmd;
                if (cpass.queue_type == ERGQueueType::TRANSFER && transfer_cmd != VK_NULL_HANDLE)
                    pass_cmd = transfer_cmd;

                submit_barriers(pass_cmd, pass_idx, cpass);
                if (const auto* span = compiled_span_for_pass(pass_idx))
                {
                    mixed_replay_state.current_pass = UINT32_MAX;
                    mixed_replay_state.current_layout = VK_NULL_HANDLE;
                    replayExecutionRange(pass_cmd, compiled_graph, record_context, frame_ctx,
                                         state.physical_resources, gpu_mgr,
                                         mixed_replay_state,
                                         span->first_command,
                                         span->one_past_last_command,
                                         false,
                                         true);
                }
                else if (cpass.pass->recorder || cpass.pass->kernel_fn)
                {
                    writePassTimestamp(
                        pass_cmd,
                        record_context,
                        compiled_graph,
                        frame_ctx.frame_index,
                        pass_idx,
                        true
                    );
                    recordPassContent(pass_cmd, cpass, compiled_graph, record_context, frame_ctx,
                                      gpu_mgr, VK_PIPELINE_BIND_POINT_COMPUTE, {});
                    writePassTimestamp(
                        pass_cmd,
                        record_context,
                        compiled_graph,
                        frame_ctx.frame_index,
                        pass_idx,
                        false
                    );
                }
                processed_passes[pass_idx] = 1;
            }
            // ===========================
            // Case 2: Graphics Pass (RenderPass Group)
            // ===========================
            else if (cpass.pass->type == ERGPassType::GRAPHICS)
            {
                uint32_t group_idx = compiled_graph.render_pass_layout.pass_to_group[pass_idx];

                if (group_idx == std::numeric_limits<uint32_t>::max()) {
                    continue;
                }

                const auto& group = compiled_graph.render_pass_layout.groups[group_idx];

                // F: Barriers are submitted per-pass, skipping conditionally-disabled passes.

                // Determine loadOps for this group's attachments (pre-computed at compile time).
                const VkAttachmentLoadOp group_color_op = group.color_load_op;
                const VkAttachmentLoadOp group_depth_op = group.depth_load_op;

                if (record_context.use_dynamic_rendering && group.local_read)
                {
                    // ===== Local-read merged scope (serial path) =====
                    // ONE vkCmdBeginRendering over the attachment UNION; per
                    // sub-pass location/input remaps + by-region barrier via the
                    // shared helper. Barriers of non-first passes were HOISTED
                    // to the first pass at compile time, so submitting every
                    // pass's (mostly empty) list up front keeps them all
                    // outside the scope.
                    for (const auto& p : group.passes)
                        submit_barriers(cmd, p.pass_index,
                                        compiled_graph.compiled_passes[p.pass_index]);

                    const uint32_t input_mask  = group.lr_input_mask;
                    const bool     depth_input = group.lr_depth_input;

                    auto resolve_view = [&](uint32_t res) -> VkImageView {
                        if (res < record_context.per_frame_views.size())
                        {
                            const auto& fv = record_context.per_frame_views[res];
                            if (frame_ctx.frame_index < fv.size())
                                return fv[frame_ctx.frame_index];
                        }
                        return VK_NULL_HANDLE;
                    };

                    std::array<VkRenderingAttachmentInfo, RenderPassKey::kMaxColorAttachments> color_attachments{};
                    const uint32_t union_count =
                        static_cast<uint32_t>(group.union_color_res.size());
                    for (uint32_t s = 0; s < union_count; ++s)
                    {
                        auto& ca = color_attachments[s];
                        ca.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                        ca.imageLayout = ((input_mask >> s) & 1u)
                            ? VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ
                            : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        ca.loadOp      = s < group.union_color_load_ops.size()
                            ? group.union_color_load_ops[s]
                            : group_color_op;
                        ca.storeOp     = s < group.union_color_store_ops.size()
                            ? group.union_color_store_ops[s]
                            : VK_ATTACHMENT_STORE_OP_STORE;
                        ca.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
                        ca.imageView   = resolve_view(group.union_color_res[s]);
                    }

                    VkRenderingAttachmentInfo depth_attachment{};
                    depth_attachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    depth_attachment.imageLayout = depth_input
                        ? VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ
                        : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    depth_attachment.loadOp      = group_depth_op;
                    depth_attachment.storeOp     = group.union_depth_store_op;
                    depth_attachment.clearValue.depthStencil = {1.0f, 0};
                    if (group.union_depth_res != std::numeric_limits<uint32_t>::max())
                        depth_attachment.imageView = resolve_view(group.union_depth_res);

                    VkRenderingInfo rendering_info{};
                    rendering_info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
                    rendering_info.renderArea.extent    = record_context.group_extents[group_idx];
                    rendering_info.layerCount           = record_context.group_layer_counts[group_idx];
                    rendering_info.colorAttachmentCount = union_count;
                    rendering_info.pColorAttachments    = union_count > 0 ? color_attachments.data() : nullptr;
                    rendering_info.pDepthAttachment     =
                        depth_attachment.imageView != VK_NULL_HANDLE ? &depth_attachment : nullptr;

                    vkCmdBeginRendering(cmd, &rendering_info);
                    for (const auto& p : group.passes)
                    {
                        const RGCompiledPass& sub_cpass =
                            compiled_graph.compiled_passes[p.pass_index];
                        const bool skip_subpass = cond_skip[p.pass_index] != 0;
                        if (!skip_subpass)
                        {
                            std::array<uint32_t, RenderPassKey::kMaxColorAttachments> locs;
                            std::array<uint32_t, RenderPassKey::kMaxColorAttachments> inps;
                            locs.fill(VK_ATTACHMENT_UNUSED);
                            inps.fill(VK_ATTACHMENT_UNUSED);
                            for (uint32_t s = 0; s < p.color_locations.size()
                                 && s < RenderPassKey::kMaxColorAttachments; ++s)
                                locs[s] = p.color_locations[s];
                            for (uint32_t s = 0; s < p.input_indices.size()
                                 && s < RenderPassKey::kMaxColorAttachments; ++s)
                                inps[s] = p.input_indices[s];
                            applyLocalReadBoundaryState(
                                cmd, union_count,
                                locs.data(), inps.data(), p.depth_input_index,
                                /*emit_barrier=*/p.subpass_index > 0);

                            if (const auto* span = compiled_span_for_pass(p.pass_index))
                            {
                                mixed_replay_state.current_pass   = UINT32_MAX;
                                mixed_replay_state.current_layout = VK_NULL_HANDLE;
                                replayExecutionRange(cmd, compiled_graph, record_context, frame_ctx,
                                                     state.physical_resources, gpu_mgr,
                                                     mixed_replay_state,
                                                     span->first_command,
                                                     span->one_past_last_command,
                                                     false,
                                                     false);
                            }
                            else
                            {
                                writePassTimestamp(
                                    cmd,
                                    record_context,
                                    compiled_graph,
                                    frame_ctx.frame_index,
                                    p.pass_index,
                                    true
                                );
                                recordPassContent(cmd, sub_cpass, compiled_graph, record_context, frame_ctx,
                                                  gpu_mgr, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  record_context.group_extents[group_idx]);
                                writePassTimestamp(
                                    cmd,
                                    record_context,
                                    compiled_graph,
                                    frame_ctx.frame_index,
                                    p.pass_index,
                                    false
                                );
                            }
                        }
                        processed_passes[p.pass_index] = 1;
                    }
                    vkCmdEndRendering(cmd);
                }
                else if (record_context.use_dynamic_rendering)
                {
                    // ===== Dynamic Rendering serial path (Vulkan 1.3) =====
                    // Each pass gets its own vkCmdBeginRendering scope. loadOps
                    // come from per-(pass, resource) first-writer bookkeeping
                    // (pass_color_load_ops): group members merge by format key
                    // and may each write a different attachment, so the group
                    // op only serves as a fallback for degenerate passes.
                    for (size_t i = 0; i < group.passes.size(); ++i)
                    {
                        uint32_t sub_pass_logic_idx = group.passes[i].pass_index;
                        const RGCompiledPass& sub_cpass = compiled_graph.compiled_passes[sub_pass_logic_idx];

                        const bool skip_subpass = cond_skip[sub_pass_logic_idx] != 0;

                        if (skip_subpass) {
                            // 被跳过的 pass 若静态持有 CLEAR,义务转移给下一个写者。
                            const auto& gp = group.passes[i];
                            if (sub_cpass.pass)
                            {
                                uint32_t decl = 0;
                                for (const auto& tr : sub_cpass.pass->textures)
                                {
                                    if (tr.role == lux::render::ETextureRole::COLOR_ATTACHMENT)
                                    {
                                        if (decl < gp.pass_color_load_ops.size() &&
                                            gp.pass_color_load_ops[decl] == VK_ATTACHMENT_LOAD_OP_CLEAR)
                                            pending_clear_resources.push_back(tr.resource.index);
                                        ++decl;
                                    }
                                    else if (tr.role == lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT &&
                                             gp.pass_depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
                                        pending_clear_resources.push_back(tr.resource.index);
                                }
                            }
                        }

                        if (!skip_subpass) {
                            std::array<VkRenderingAttachmentInfo, RenderPassKey::kMaxColorAttachments> color_attachments{};
                            VkRenderingAttachmentInfo depth_attachment{};
                            fillAttachmentInfos(record_context, sub_cpass, group,
                                                &group.passes[i],
                                                frame_ctx.frame_index, group_color_op, group_depth_op,
                                                color_attachments, depth_attachment,
                                                &pending_clear_resources);

                            VkRenderingInfo rendering_info{};
                            rendering_info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
                            rendering_info.renderArea.extent    = record_context.group_extents[group_idx];
                            rendering_info.layerCount           = record_context.group_layer_counts[group_idx];
                            rendering_info.colorAttachmentCount = group.key.color_count;
                            rendering_info.pColorAttachments    = group.key.color_count > 0 ? color_attachments.data() : nullptr;
                            rendering_info.pDepthAttachment     = (group.key.depth_stencil_format != VK_FORMAT_UNDEFINED && depth_attachment.imageView != VK_NULL_HANDLE) ? &depth_attachment : nullptr;

                            submit_barriers(cmd, sub_pass_logic_idx, sub_cpass);
                            vkCmdBeginRendering(cmd, &rendering_info);
                            if (const auto* span = compiled_span_for_pass(sub_pass_logic_idx))
                            {
                                mixed_replay_state.current_pass = UINT32_MAX;
                                mixed_replay_state.current_layout = VK_NULL_HANDLE;
                                replayExecutionRange(cmd, compiled_graph, record_context, frame_ctx,
                                                     state.physical_resources, gpu_mgr,
                                                     mixed_replay_state,
                                                     span->first_command,
                                                     span->one_past_last_command,
                                                     false,
                                                     false);
                            }
                            else
                            {
                                writePassTimestamp(
                                    cmd,
                                    record_context,
                                    compiled_graph,
                                    frame_ctx.frame_index,
                                    sub_pass_logic_idx,
                                    true
                                );
                                recordPassContent(cmd, sub_cpass, compiled_graph, record_context, frame_ctx,
                                                  gpu_mgr, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  record_context.group_extents[group_idx]);
                                writePassTimestamp(
                                    cmd,
                                    record_context,
                                    compiled_graph,
                                    frame_ctx.frame_index,
                                    sub_pass_logic_idx,
                                    false
                                );
                            }
                            vkCmdEndRendering(cmd);
                        }

                        processed_passes[sub_pass_logic_idx] = 1;
                    }
                }
                // Legacy VkRenderPass/VkFramebuffer path removed.
                // The engine requires dynamic rendering (Vulkan 1.3 / VK_KHR_dynamic_rendering).
            }
        }

        // Finalize async command buffers and populate multi-queue submit info.
        buildMultiQueueSubmit(record_context, compiled_graph, cmd, compute_cmd, transfer_cmd);

        } // end slow path

        // Emit graph-export barriers (e.g., swapchain PRESENT_SRC_KHR transition).
        submitFinalBarriers(cmd, compiled_graph, state.physical_resources, record_context,
                            frame_ctx.frame_index);

        // Clear injected external slot views AND images so they are not used by a
        // later record(). Imported slot views/images (e.g. from OffscreenRenderTarget)
        // are owned by the calling code — both are re-injected on every record(), so
        // zeroing them here is safe. Clearing the view but NOT the image left a stale
        // VkImage override that resolveImageHandle() (which checks per_frame_images
        // before the physical table) could feed into barriers after the external
        // image was torn down — emitting a transition against a dead image. (P1#19)
        for (size_t si = 0; si < kTargetSlotCount; ++si)
        {
            const uint32_t ri = compiled_graph.slot_resource_idx[si];
            if (ri == RGCompiledGraph::kInvalidSlotIdx) continue;
            if (ri < record_context.per_frame_views.size() &&
                frame_ctx.frame_index < record_context.per_frame_views[ri].size())
                record_context.per_frame_views[ri][frame_ctx.frame_index] = VK_NULL_HANDLE;
            if (ri < record_context.per_frame_images.size() &&
                frame_ctx.frame_index < record_context.per_frame_images[ri].size())
                record_context.per_frame_images[ri][frame_ctx.frame_index] = VK_NULL_HANDLE;
        }
    }

    // ================================================================
    // executeFast — 字节码快路执行器(对应 interpret 的 lambda 解释路径)
    // ================================================================
    // Linearly replays the pre-compiled ExecutionProgram, bypassing
    // recorder-lambda dispatch entirely.  Only used when all passes
    // declare a kernel and no runtime conditions exist.
