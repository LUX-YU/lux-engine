#include <lux/engine/render/renderer/FrameOrchestrator.hpp>

#include <lux/engine/render/renderer/Renderer.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>
#include <lux/engine/render/targets/OffscreenImagePool.hpp> // applyLayout
#include <lux/engine/render/gpu/RenderContext.hpp>          // reportError(补槽上报)

namespace lux::render
{

    void FrameOrchestrator::beginFrame(Renderer& renderer) const
    {
        renderer.beginFrame(current_stamp_);
    }

    void FrameOrchestrator::endFrame(Renderer& renderer) const
    {
        renderer.endFrame(current_stamp_.serial);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  阶段 1:开帧
    // ─────────────────────────────────────────────────────────────────────────

    Expected<FrameOrchestrator::EFrameStart>
    FrameOrchestrator::beginRenderFrame(RenderTargetRegistry& targets, FrameDriver* driver, FrameTickState& st)
    {
        // ── 可渲染性:一个绑了层的 Surface,或任意一个离屏 target ──────────
        st.surface_target = targets.surfaceTarget();
        const bool surface_bound = st.surface_target && !st.surface_target->layers.empty();
        bool any_offscreen = false;
        for (const auto& t : targets.all().values())
            if (t.kind == RenderTargetEntry::EKind::Offscreen)
            {
                any_offscreen = true;
                break;
            }
        if (!any_offscreen && !surface_bound)
            return EFrameStart::NoTarget;

        // ── 重建扫描:所有需要重建的 Surface target ────────────────────────
        //
        // 必须先于 FrameDriver::beginFrame —— 重建要 waitAllFences,而开帧会 reset
        // 当前槽的 fence,帧中再等就死锁。
        //
        // 这段原本只存在于 UI 的 tick 里,看着像"副视口特有";实际它是
        // **FrameDriver 只认识传给它的那一个 present** 的补偿 —— 主窗由
        // FrameDriver 内部重建,其余 Surface target 无人过问。这里一视同仁:
        // 主窗那份因此变成 no-op(needsRebuild 已假),多窗也不再需要外部代劳。
        if (driver)
            for (auto& t : targets.all().values())
            {
                if (t.kind != RenderTargetEntry::EKind::Surface || !t.present)
                    continue;
                if (t.rebuild_suspended) // 改尺寸括号期内挂起(见字段说明)
                    continue;
                if (!t.present->needsRebuild())
                    continue;
                auto waited = driver->waitAllFences();
                if (!waited)
                    return lux::cxx::unexpected<RenderError>(waited.error());
                auto rebuilt = t.present->rebuild();
                if (!rebuilt && !detail::isRetryableSwapchainFailure(rebuilt.error()))
                {
                    return lux::cxx::unexpected<RenderError>(rebuilt.error());
                }
                // Only the typed surface-churn/unavailable errors are explicitly
                // safe to retry next frame. Every other failure stops the device
                // session through this Expected.
            }

        // ── 开帧 ──────────────────────────────────────────────────────────
        //
        // 是否参与呈现,以**绑了层的 Surface** 为准:没有绑定就什么都不会渲进
        // acquire 到的图像,却照样呈现一张 UNDEFINED 布局的图(validation 报错),
        // 且 acquire 信号量的 signal 无人消费 —— 下一次同槽 acquire 就会撞上
        // "semaphore has pending operations",fence/命令缓冲的节奏随之崩掉
        // (scene_cycle_stress_test 抓到过)。无绑定的帧走 FrameDriver 的纯离屏路径。
        st.surface_present = surface_bound ? st.surface_target->present.get() : nullptr;

        st.rt = FrameRuntime{};
        st.rt.stamp = current_stamp_;

        if (driver)
        {
            auto runtime = driver->beginFrame(current_stamp_.slotIndex(), current_stamp_.serial, st.surface_present);
            if (!runtime)
                return lux::cxx::unexpected<RenderError>(runtime.error());
            st.rt = std::move(*runtime);
            patchImageIndex(st.rt.image_index);
            st.rt.stamp = current_stamp_; // FrameDriver 填完其余字段后重挂 stamp

            if (st.rt.primary_cmd == VK_NULL_HANDLE)
                return EFrameStart::Skip; // 最小化 / 重建失败 —— 跳过本帧
        }

        // swapchain 的呈现绑定:存在 FrameTickState 里,好活过 renderTargets。
        if (st.surface_present)
        {
            st.present_binding = st.surface_present->provider()->makeFrameBinding(st.rt.image_index);
            st.rt.present_target = &st.present_binding;
        }

        // ── 其余 Surface target:各自 acquire,信号量折入本帧提交 ─────────────
        //
        // 主 Surface 由 FrameDriver 内部 acquire(fence/命令缓冲归它);其余的在
        // 这里补齐。它们的绘制照样录进同一个 primary_cmd,所以只有一次提交;
        // present 排在结帧之后(见 endRenderFrame)。
        st.extra_surfaces.clear();
        if (st.rt.primary_cmd != VK_NULL_HANDLE)
            for (auto& t : targets.all().values())
            {
                if (t.kind != RenderTargetEntry::EKind::Surface || !t.present)
                    continue;
                if (&t == static_cast<const RenderTargetEntry*>(st.surface_target))
                    continue; // 主 Surface 已由 FrameDriver 处理
                if (t.layers.empty())
                    continue; // 没有层可画,不必 acquire
                if (t.present->needsRebuild())
                    continue; // 帧中不 waitAllFences —— 下帧前段重建

                auto acquired = t.present->acquire();
                if (!acquired)
                    return lux::cxx::unexpected<RenderError>(acquired.error());
                const auto& acq = *acquired;
                if (!acq.valid)
                    continue; // 失败已按语义标记重建,跳过本帧

                FrameTickState::AcquiredSurface s{};
                s.present = t.present.get();
                s.binding = t.present->provider()->makeFrameBinding(acq.image_index);
                s.image_index = acq.image_index;
                s.present_sem = acq.present_sem;
                s.target = &t;

                VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                wait.semaphore = acq.acquire_sem;
                wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                st.rt.external_graphics_waits.push_back(wait);

                VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                sig.semaphore = acq.present_sem;
                sig.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
                st.rt.external_graphics_signals.push_back(sig);

                st.extra_surfaces.push_back(std::move(s));
            }

        return EFrameStart::Ready;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  阶段 2:渲染
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        /// 按层在链中的位置,从 target 的布局导出该层实际该用的布局。
        ///
        /// **单层链(首层且末层)的结果与原布局逐字相同** —— 这保证了引入相位计算
        /// 对既有单层路径是零行为变化。
        RenderTargetLayout layoutForPhase(const RenderTargetLayout& base, const LayerPhase& phase)
        {
            RenderTargetLayout out = base;
            for (auto& slot : out.slots)
            {
                if (!slot.has_value())
                    continue;
                // 非首层:图像已有内容,LOAD 而非 CLEAR。
                slot->preserve_content = !phase.is_first;
                if (!phase.is_last)
                {
                    // 非末层:把图像交棒给下一层,不做终态转换。
                    slot->final_state = ERenderResourceState::COLOR_ATTACHMENT;
                    slot->is_presentable = false;
                }
            }
            return out;
        }
    } // namespace

    void FrameOrchestrator::renderTargets(
        RenderTargetRegistry& targets,
        Renderer& renderer,
        SceneViewBatch& batch,
        FrameTickState& st
    )
    {
        using Layer = RenderTargetEntry::CompositeLayer;
        const auto& stamp = current_stamp_;

        // 上传:全局 + 各场景的传输,每 tick 一次。
        if (st.rt.primary_cmd != VK_NULL_HANDLE)
            renderer.runUploadPhase(st.rt.primary_cmd);

        // 一条合成链的执行:逐层按位置算相位,再按层的形态分发。
        // 链只算**决策**,不接管**执行** —— 场景层的 BeginRendering/屏障由渲染图
        // 拥有,自定义层由它自己的录制回调拥有;链能统一的是"你是第几层"。
        auto runChain = [&](const RenderTargetEntry& t, RenderTargetBinding& binding) {
            const size_t n = t.layers.size();
            for (size_t i = 0; i < n; ++i)
            {
                const auto& l = t.layers[i];
                const LayerPhase phase{
                    .is_first = (i == 0),
                    .is_last = (i + 1 == n),
                };
                const RenderTargetLayout phased = layoutForPhase(t.layout, phase);
                binding.layout = &phased;

                if (l.kind == Layer::EKind::SceneView)
                {
                    auto* scene = renderer.getScene(l.scene_id);
                    auto* view = scene ? scene->getView(l.view_id) : nullptr;
                    if (!scene || !view)
                        continue;
                    batch.add(scene, view);
                    const auto& item = batch.items().back();
                    // 无 FrameDriver 的 headless tick 里 primary_cmd 为空(上面的
                    // 上传同理已守卫)。跳过录制,而不是对空命令缓冲发
                    // BeginRendering/屏障。
                    if (st.rt.primary_cmd != VK_NULL_HANDLE)
                        renderer.renderSingleView(*item.scene, *item.view, binding, st.rt, item.cross_view_index);
                }
                else if (l.record && st.rt.primary_cmd != VK_NULL_HANDLE)
                {
                    l.record(l.user, st.rt.primary_cmd, binding, phase);
                }
            }
            binding.layout = nullptr; // phased 是栈上的,别把悬垂指针留给调用方
        };

        // 特性声明的额外输出槽 —— 在(目标,场景)相遇处补齐。
        // 掩码并集来自该目标全部 SceneView 层的场景;缺槽时按 defaultTargetSlotDesc
        // 同时写进**条目布局**(相位/重编判定用它)与**池布局**(applyLayout 重建
        // 物理背衬,旧影像照旧 retire/GC)。下游 prepareSceneForRender 的 format-key
        // 无条件比较自动按新布局重编图模板,SceneGraphCache 照常导入。
        // 稳态成本 = 每目标每 tick 一次掩码比较;命中(装了新声明槽位的特性)才动重建。
        // v1 仅离屏目标(编辑器主视图即是);纯 Surface 路径的额外槽待池侧配套。
        auto amendTargetSlots = [&](RenderTargetEntry& t) {
            using Layer = RenderTargetEntry::CompositeLayer;
            uint32_t need = 0;
            for (const auto& l : t.layers)
                if (l.kind == Layer::EKind::SceneView)
                    if (auto* scene = renderer.getScene(l.scene_id))
                        need |= scene->requiredTargetSlotMask();

            const uint32_t have = targetSlotMask(t.layout);
            const uint32_t missing = need & ~have;

            // 对"已存在且被声明需要"的槽追加 SAMPLED —— 声明需要一个
            // 已有的槽,含义就是要**读**它(典型:LinearDepth 解算要采样
            // SceneDepth,而主深度槽默认只有 DEPTH_STENCIL_ATTACHMENT)。
            // usage 不入 format-key,无需图重编,只重建池影像。
            uint32_t usage_amended = 0;
            for (size_t i = 0; i < kTargetSlotCount; ++i)
            {
                if (!((need & have) & (1u << i)))
                    continue;
                auto& d = *t.layout.slots[i]; // have 位已保证 optional 有值
                if (hasUsage(d.usage, ERenderImageUsage::SAMPLED))
                    continue;
                d.usage |= ERenderImageUsage::SAMPLED;
                usage_amended |= 1u << i;
                // 上层给的布局没打开 SAMPLED,引擎替它打开了 —— 能跑,但上层拿到的
                // 目标与它以为的不同,该让它看见。
                renderer.renderContext().reportError(
                    renderError<err::frame::TargetLayoutAmended>(static_cast<std::uint32_t>(i)),
                    RenderErrorEvent::kNoScene,
                    current_stamp_.serial
                );
            }

            if (missing == 0 && usage_amended == 0)
                return;

            for (size_t i = 0; i < kTargetSlotCount; ++i)
            {
                if (!(missing & (1u << i)))
                    continue;
                const auto slot = static_cast<TargetSlot>(i);
                const auto d = defaultTargetSlotDesc(slot);
                if (d.format == lux::rdesc::ETextureFormat::UNDEFINED)
                    continue; // 主槽/未知槽不经形状表
                t.layout.slots[i] = d;
                renderer.renderContext().reportError(
                    renderError<err::frame::TargetLayoutAmended>(static_cast<std::uint32_t>(i)),
                    RenderErrorEvent::kNoScene,
                    current_stamp_.serial
                );
            }
            t.pool->applyLayout(t.layout);
        };

        // 离屏 target。批在**所有链之前**清空,免得上一帧的场景/视图指针残留。
        // (values() 是容器级 const-only,补槽需要可变条目 —— 改走
        //  keys()+tryGet 的变更惯用法,与 UI 侧一致;两数组平行,遍历序不变。)
        batch.clear();
        for (const auto id : targets.all().keys())
        {
            auto* t = targets.tryGet(id);
            if (!t || t->kind != RenderTargetEntry::EKind::Offscreen || !t->pool)
                continue;
            amendTargetSlots(*t);
            auto binding = t->pool->makeFrameBinding(stamp.slotIndex());
            runChain(*t, binding);
        }

        // 主 Surface。
        if (st.surface_present && st.surface_target && st.rt.present_target)
            runChain(*st.surface_target, *st.rt.present_target);

        // 其余已 acquire 的 Surface(多窗)—— 同一命令缓冲,同一次提交。
        for (auto& s : st.extra_surfaces)
            if (s.target)
                runChain(*s.target, s.binding);

        // 被触及的场景收尾。
        for (auto* scene : batch.touchedScenes())
            scene->endViewFrame(stamp.slotIndex());
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  阶段 3:结帧
    // ─────────────────────────────────────────────────────────────────────────

    Expected<void>
    FrameOrchestrator::endRenderFrame(RenderTargetRegistry& targets, FrameDriver* driver, FrameTickState& st)
    {
        if (driver)
        {
            auto ended = driver->endFrame(st.rt, st.surface_present);
            if (!ended)
                return lux::cxx::unexpected<RenderError>(ended.error());
        }

        // 其余 Surface 的 present:提交已在 endFrame 内折入各自的 present 信号,
        // 这里逐个呈现。可恢复的 OUT_OF_DATE/SUBOPTIMAL/SURFACE_LOST 已由
        // PresentContext 归一为重建标记；仍返回错误的就是 device session 不能
        // 继续吞掉的永久失败，必须与主 Surface 一样沿 Expected 交给宿主停机。
        for (auto& s : st.extra_surfaces)
            if (s.present)
            {
                auto presented = s.present->present(s.image_index, s.present_sem);
                if (!presented)
                    return lux::cxx::unexpected<RenderError>(presented.error());
            }
        st.extra_surfaces.clear();

        // 目标池按**栅栏证实**的完成水位老化回收。
        const uint64_t gpu_completed = driver ? driver->gpuCompletedSerial() : current_stamp_.serial;
        for (auto& t : targets.all().values())
            if (t.pool)
                t.pool->collectRetired(current_stamp_.serial, gpu_completed);
        return {};
    }

} // namespace lux::render
