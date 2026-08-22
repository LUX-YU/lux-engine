#include <lux/engine/render/scene/SceneGraphCache.hpp>

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/utils/FormatMap.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGDebugPrint.hpp>
#include <lux/engine/render/graph/RGVulkanRecorder.hpp>
#include <lux/engine/render/graph/RGVulkanResourceAllocator.hpp>
#include <lux/engine/render/graph/RenderGraphCompiler.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/targets/RenderTargetBinding.hpp>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <utility>

namespace lux::render
{
    SceneGraphCache::SceneGraphCache(RenderContext& ctx, std::string debug_name)
        : ctx_(ctx)
        , debug_name_(std::move(debug_name))
    {
        auto& res_ctx = ctx_.resourceContext();
        allocator_ = std::make_unique<RGVulkanResourceAllocator>(res_ctx);
        recorder_  = std::make_unique<RGVulkanRecorder>(res_ctx, ctx_.pipelineManager());
    }

    SceneGraphCache::~SceneGraphCache()
    {
        shutdown();
    }

    RGVulkanResourceAllocator& SceneGraphCache::allocator() noexcept { return *allocator_; }
    RGVulkanRecorder&          SceneGraphCache::recorder()  noexcept { return *recorder_; }

    void SceneGraphCache::invalidate(
        EGraphInvalidationReason reason) noexcept
    {
        state_.valid = false;
        const auto bits = static_cast<std::uint32_t>(reason);
        telemetry_.pending_invalidation_bits |= bits;
        for (std::size_t index = 0u;
             index < telemetry_.invalidation_counts.size(); ++index)
        {
            if ((bits & (1u << index)) != 0u)
                ++telemetry_.invalidation_counts[index];
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  编译(事务式 build-then-commit)
    // ─────────────────────────────────────────────────────────────────────

    void SceneGraphCache::compile(const RenderTargetLayout&  layout,
                                  std::uint32_t              required_target_slots,
                                  const PassContribution&    contribute_passes,
                                  std::span<View* const>     all_views,
                                  SceneDomainDescriptorSets* domain_sets,
                                  std::uint64_t              frame_serial)
    {
        const auto total_started = std::chrono::steady_clock::now();
        ++telemetry_.compile_attempts;
        std::uint64_t build_nanoseconds = 0u;
        std::uint64_t compile_nanoseconds = 0u;
        const auto finishAttempt = [&](bool succeeded)
        {
            const auto total_nanoseconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - total_started).count());
            telemetry_.build_nanoseconds += build_nanoseconds;
            telemetry_.compile_nanoseconds += compile_nanoseconds;
            telemetry_.total_nanoseconds += total_nanoseconds;
            if (succeeded)
                ++telemetry_.compile_successes;
            else
                ++telemetry_.compile_failures;
            compile_history_.push_back(SceneGraphCompileSample{
                frame_serial,
                telemetry_.pending_invalidation_bits,
                build_nanoseconds,
                compile_nanoseconds,
                total_nanoseconds,
                succeeded});
            constexpr std::size_t kCompileHistoryCapacity = 4096u;
            if (compile_history_.size() > kCompileHistoryCapacity)
                compile_history_.pop_front();
            if (succeeded)
                telemetry_.pending_invalidation_bits = 0u;
        };
        // ── 先把新图完整编出来,放进局部变量,**完全不触碰**当前在用的图与视图资源。
        // 没有颜色目标的布局 -> 不出图,是一个合法的"不渲染"结果。
        // 编译失败则一路 return,什么都没改,场景继续用上一份好图(旧实现先退休旧图
        // 再编,失败即黑屏)。
        std::unique_ptr<RGCompiledGraph> new_graph;
        RGResourceHandle                 new_color_handle{};

        if (layout.hasSlot(TargetSlot::SCENE_COLOR))
        {
            const auto& color_slot = layout.slot(TargetSlot::SCENE_COLOR);

            RGBuilder builder;

            // 未知 VkFormat 必须中止编译,不能静默回落 —— 回落到"合法但错误"的格式会
            // 绕过 RenderPassPlanner 的 VK_FORMAT_UNDEFINED 报错门,把错格式烙进
            // VkPipeline(dynamic rendering 格式失配,无诊断)。
            const auto color_fmt = color_slot.format;
            if (color_fmt == lux::rdesc::ETextureFormat::UNDEFINED)
            {
                ctx_.reportError(renderError<err::graph::SlotFormatUnmapped>(
                                     static_cast<std::uint32_t>(TargetSlot::SCENE_COLOR),
                                     static_cast<std::uint32_t>(color_slot.format)),
                                 scene_index_);
                finishAttempt(false);
                return;
            }

            // 模板用 1x1 占位尺寸 —— 真实图像每帧经 imported_slots 注入。
            RGTextureDescription bb_desc = RGTextureDescription::Absolute(1, 1, color_fmt);

            RGImportedResourceInfo bb_import{};
            bb_import.image_getter     = nullptr; // no getter; images injected via imported_slots
            bb_import.update_group     = static_cast<uint32_t>(RGUpdateGroup::GROUP_SWAPCHAIN);
            bb_import.initial_layout =
                toVkImageLayout(color_slot.initial_state);
            bb_import.final_layout =
                toVkImageLayout(color_slot.final_state);
            bb_import.preserve_content = color_slot.preserve_content;
            // Handover stage: the stage at or after which the PREVIOUS holder of this
            // image is done reading it. For a swapchain image that holder is the
            // presentation engine, and FrameDriver waits the acquire semaphore at
            // COLOR_ATTACHMENT_OUTPUT — so the graph's first layout transition must sit
            // in that same stage, or it escapes the semaphore gate and overwrites an
            // image still being scanned out (SYNC-HAZARD-WRITE-AFTER-READ).
            //
            // Declared unconditionally rather than keyed off is_presentable: that flag
            // is deliberately excluded from the format key (one template serves both a
            // swapchain and an offscreen binding), so the template cannot depend on it.
            // An offscreen target's previous holder is the previous frame's writer,
            // which is also COLOR_ATTACHMENT_OUTPUT.
            bb_import.initial_stage    = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

            auto backbuffer = builder.importSlottedTexture(
                TargetSlot::SCENE_COLOR, "SceneColor", bb_desc, bb_import);

            RGTextureDescription depth_desc = RGTextureDescription::Relative(
                1.0f, 1.0f, lux::rdesc::ETextureFormat::D32_SFLOAT);
            depth_desc.usage = static_cast<uint32_t>(ERGTextureUsageBits::DEPTH_STENCIL)
                             | static_cast<uint32_t>(ERGTextureUsageBits::SAMPLED)
                             // INPUT_ATTACHMENT: the local-read merged scope reads depth
                             // as an input attachment (line-B design P2 item ⑤ — the
                             // three G-buffer colors already carry the bit).
                             | static_cast<uint32_t>(ERGTextureUsageBits::INPUT_ATTACHMENT);
            (void)builder.createTexture("SceneDepth", depth_desc);

            // Offscreen targets receive feature-declared auxiliary slots from
            // FrameOrchestrator and import their pool-backed images below. A
            // present surface cannot grow swapchain images with arbitrary
            // attachments, but its scene graph still needs those resources
            // (LinearDepth is consumed by Fog/Water, for example). Supply a
            // graph-local transient for every declared slot absent from the
            // target layout. The stable slot name lets feature references
            // resolve identically on both paths; only external readback differs.
            const std::uint32_t present_slots = targetSlotMask(layout);
            const std::uint32_t transient_slots =
                required_target_slots & ~present_slots;
            for (std::size_t si = 0; si < kTargetSlotCount; ++si)
            {
                if ((transient_slots & (1u << si)) == 0u)
                    continue;
                const auto slot = static_cast<TargetSlot>(si);
                if (slot == TargetSlot::SCENE_COLOR ||
                    slot == TargetSlot::SCENE_DEPTH ||
                    slot == TargetSlot::RESOLVE_COLOR)
                {
                    continue;
                }
                const auto slot_desc = defaultTargetSlotDesc(slot);
                if (slot_desc.format ==
                    lux::rdesc::ETextureFormat::UNDEFINED)
                {
                    continue;
                }
                auto transient_desc = RGTextureDescription::Relative(
                    1.0f,
                    1.0f,
                    slot_desc.format);
                transient_desc.usage =
                    static_cast<ERGTextureUsageFlags>(
                        ERGTextureUsageBits::COLOR_ATTACHMENT) |
                    static_cast<ERGTextureUsageFlags>(
                        ERGTextureUsageBits::SAMPLED);
                (void)builder.createTexture(
                    targetSlotName(slot),
                    transient_desc);
            }

            // Import every ADDITIONAL semantic slot the layout declares (beyond the
            // framework-special SceneColor / SceneDepth / ResolveColor) as an
            // externally-accessible slotted texture (P4b). A feature writes one
            // via referenceTexture(targetSlotName(slot)); its physical image is
            // allocated by the OffscreenImagePool and injected per-frame through the
            // binding — so the output is readable-back / bindable, not a graph-internal
            // transient. No-op for a layout that declares only the primary slots.
            for (size_t si = 0; si < kTargetSlotCount; ++si)
            {
                const auto extra_slot = static_cast<TargetSlot>(si);
                if (extra_slot == TargetSlot::SCENE_COLOR ||
                    extra_slot == TargetSlot::SCENE_DEPTH ||
                    extra_slot == TargetSlot::RESOLVE_COLOR)
                    continue;                      // primary slots handled above
                if (!layout.hasSlot(extra_slot))
                    continue;
                const auto& sd = layout.slot(extra_slot);
                RGImportedResourceInfo imp{};
                imp.slot             = extra_slot;
                imp.update_group     = static_cast<uint32_t>(RGUpdateGroup::GROUP_SWAPCHAIN);
                imp.initial_layout = toVkImageLayout(sd.initial_state);
                imp.final_layout = toVkImageLayout(sd.final_state);
                imp.preserve_content = sd.preserve_content;
                const auto extra_fmt = sd.format;
                if (extra_fmt == lux::rdesc::ETextureFormat::UNDEFINED)
                {
                    ctx_.reportError(renderError<err::graph::SlotFormatUnmapped>(
                                         static_cast<std::uint32_t>(extra_slot),
                                         static_cast<std::uint32_t>(sd.format)),
                                     scene_index_);
                    finishAttempt(false);
                    return;   // 同上:事务式,未改动任何状态
                }
                RGTextureDescription st = RGTextureDescription::Absolute(1, 1, extra_fmt);
                (void)builder.importSlottedTexture(extra_slot, targetSlotName(extra_slot), st, imp);
            }

            // 让场景把该出的 pass 声明进来。本类不认识特性 —— 谁去声明、
            // 按什么顺序,由场景决定(见 SceneFeatureSet::contributePasses)。
            const auto build_started = std::chrono::steady_clock::now();
            if (contribute_passes)
                contribute_passes(builder);

            // 静态编译(不分配任何物理资源)。域 set 实例随图一起传入:切到合并布局的
            // 管线后,其 FEATURE 域槽在录制期绑定到该实例(绑定计划把若干逻辑绑定
            // 收敛成一次域绑定)。
            RGCompileOptions compile_options{};
            compile_options.domain_sets = domain_sets;
            // local_read 合并组的 color attachment 上限必须让步于设备:Vulkan 只
            // 保证 4,而合并出的组比设备上限宽时 vkCmdBeginRendering 直接失败。
            // 这是本类第一个把 DeviceCaps 传进编译期的地方 —— 之前用的是
            // RenderPassKey 的数组容量(8),那是内存尺寸,不是硬件许可。
            compile_options.max_color_attachments =
                ctx_.deviceContext().caps().max_color_attachments;
            auto description = std::move(builder).build();
            build_nanoseconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - build_started).count());
            const auto compile_started = std::chrono::steady_clock::now();
            auto compiled = RenderGraphCompiler::compile(
                std::move(description),
                ctx_.pipelineManager(),
                compile_options);
            compile_nanoseconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - compile_started).count());

            // 编译期发现的非致命问题:图仍然编得出来,但创建者的用途声明不足之类
            // 的东西必须可见。成功与失败两条路都要上报 —— 它们与 valid 无关。
            for (const RenderError& diagnostic : compiled.diagnostics)
                ctx_.reportError(diagnostic, scene_index_);

            if (!compiled.valid)
            {
                ctx_.reportError(compiled.compile_error.ok()
                                     ? renderError<err::graph::CompiledGraphInvalid>()
                                     : compiled.compile_error,
                                 scene_index_);
                // Commit NOTHING — 旧图 + 视图资源 + last_layout 全部保持原样。
                finishAttempt(false);
                return;
            }

            new_graph        = std::make_unique<RGCompiledGraph>(std::move(compiled));
            new_color_handle = backbuffer;
        }

        // ── 提交点 ─────────────────────────────────────────────────────────
        // 到这里才动可观察状态:记录布局 -> 退休旧图(FIF 帧后延迟销毁)+ 收走按旧图
        // 分配的视图资源(让它们回到瞬态池而不是泄漏显存,P1#37)-> 装入新图
        // (无颜色布局时为 null)。
        state_.last_layout = layout;

        const RGGraphDescription* old_graph_desc = nullptr;
        if (state_.graph)
        {
            old_graph_desc = &state_.graph->original_graph;
            RetiredGraph retired;
            retired.graph        = std::move(state_.graph);
            retired.retire_frame = 0; // 由 collectRetired 打戳
            retired_graphs_.push_back(std::move(retired));
            telemetry_.retired_graph_high_water = std::max(
                telemetry_.retired_graph_high_water,
                static_cast<std::uint64_t>(retired_graphs_.size()));
        }
        retireAllViewResources(all_views, old_graph_desc);

        state_.graph              = std::move(new_graph);   // null for a no-color layout
        state_.final_color_handle = new_color_handle;
        state_.valid              = (state_.graph != nullptr);

        // 布局计划告警无条件上报:该字段只在真出问题时非空,而它描述的问题
        // (如同一 pass 内变体管线布局不兼容)否则只能靠运行期校验层暴露 ——
        // 而 Release 构建根本不开校验层。藏在调试开关后面等于没有。
        if (state_.graph && state_.graph->layout_plan)
        {
            for (const RenderError& warning : state_.graph->layout_plan->warnings)
                ctx_.reportError(warning, scene_index_);
        }
        finishAttempt(true);

        // (原先这里还有一段 LUX_DUMP_RG=1 门控的整图 dump,直接写给标准错误。
        //  printCompiledGraph 本身没问题 —— 它接受任意 std::ostream&;有问题的是
        //  **这个调用点替上层选了终端**。要看这张图,客户端发 DumpRenderGraph
        //  命令即可,文本原样写进它自己的缓冲区;编辑器已经这么用。
        //  dump(std::ostream&) 也仍在,测试直接调它。)
    }

    // ─────────────────────────────────────────────────────────────────────
    //  退休
    // ─────────────────────────────────────────────────────────────────────

    void SceneGraphCache::retireAllViewResources(std::span<View* const>    views,
                                                 const RGGraphDescription* source_graph) noexcept
    {
        for (auto* v : views)
        {
            if (v && v->resource_state)
                retired_view_resources_.push_back(
                    RetiredViewResources{std::move(v->resource_state), 0, source_graph});
        }
        telemetry_.retired_view_resource_high_water = std::max(
            telemetry_.retired_view_resource_high_water,
            static_cast<std::uint64_t>(retired_view_resources_.size()));
    }

    void SceneGraphCache::retireViewResourceState(RGResourceState&&         state,
                                                  const RGGraphDescription* source_graph)
    {
        if (state.record_ctx.frames_in_flight == 0)
            return;

        auto retired = std::make_unique<RGResourceState>(std::move(state));
        retired_view_resources_.push_back(RetiredViewResources{std::move(retired), 0, source_graph});
        telemetry_.retired_view_resource_high_water = std::max(
            telemetry_.retired_view_resource_high_water,
            static_cast<std::uint64_t>(retired_view_resources_.size()));
    }

    void SceneGraphCache::collectRetired(uint64_t frame_id, uint64_t completed_serial)
    {
        // 回收退休的每视图资源(物理图像 + 录制上下文)。
        // **必须先于** retired_graphs_ —— source_graph 指向那些图。
        for (auto& rv : retired_view_resources_)
        {
            if (rv.retire_frame == 0)
                rv.retire_frame = frame_id;   // 首次见到时打戳 = 其最后一次 GPU 使用的上界
        }
        while (!retired_view_resources_.empty())
        {
            auto& oldest = retired_view_resources_.front();
            // 只有当**栅栏证实**的完成水位越过戳记才释放。原来的
            // "frame_id - retire_frame >= fif" 算术会高估完成度:序号在不提交的
            // tick 上照样前进(见 FrameDriver::gpuCompletedSerial)。
            if (oldest.retire_frame > completed_serial)
                break;

            if (recorder_)
                recorder_->deallocateRecordContext(oldest.state->record_ctx);
            if (allocator_ && oldest.source_graph)
                allocator_->deallocateToPool(oldest.state->physical_resources, *oldest.source_graph);
            else if (allocator_)
                allocator_->deallocate(oldest.state->physical_resources);
            retired_view_resources_.pop_front();
        }

        // 回收退休图。在视图资源之后,以保持 source_graph 指针有效。
        for (auto& rg : retired_graphs_)
        {
            if (rg.retire_frame == 0)
                rg.retire_frame = frame_id;
        }
        while (!retired_graphs_.empty())
        {
            auto& oldest = retired_graphs_.front();
            if (oldest.retire_frame > completed_serial)
                break;
            // 安全释放 —— 引用该图的视图资源都已在上一段清理完毕。
            retired_graphs_.pop_front();
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  诊断 / 清理
    // ─────────────────────────────────────────────────────────────────────

    void SceneGraphCache::dump(std::ostream& os) const
    {
        if (state_.valid && state_.graph)
            printCompiledGraph(*state_.graph, os);
        else
            os << "[SceneGraphCache] '" << debug_name_
               << "' has no compiled render graph yet (not rendered, or compile failed).\n";
    }

    void SceneGraphCache::shutdown()
    {
        // 退休图:unique_ptr 自析构即可。
        retired_graphs_.clear();

        // 退休的每视图资源:必须在 recorder_/allocator_ 还活着时归还。
        for (auto& retired : retired_view_resources_)
        {
            if (retired.state)
            {
                if (recorder_)
                    recorder_->deallocateRecordContext(retired.state->record_ctx);
                if (allocator_)
                    allocator_->deallocate(retired.state->physical_resources);
            }
        }
        retired_view_resources_.clear();

        state_.graph.reset();
        state_.valid = false;

        recorder_.reset();
        allocator_.reset();
    }

} // namespace lux::render
