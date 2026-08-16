#include <lux/engine/ui/UIRenderServer.hpp>
#include <lux/engine/ui/ImGuiFeature.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>
#include <lux/engine/ui/UIOffscreenImagePool.hpp>
#include <lux/engine/ui/ImGuiDrawDataSnapshot.hpp>

// M5:此处曾是 #include <.../comm/server/RenderServerImpl.hpp> —— 基类的
// **私有** Impl 定义。UI 靠它取 targets、renderer、上下文、帧驱动、记账入口,
// 于是 Impl 的每一个字段都成了 UI 的接口:改基类的任何私有成员都可能悄悄弄坏
// UI,而编译器不会有半句话。真正的封装破口一直是这一行。
//
// 现在改走基类 protected 的扩展面(RenderServer.hpp 里那一组),UI 只认识
// 树内公开的 L4 类型:目标注册表、渲染器、帧运行态。
// (InitialViewCamera.hpp retired — initial camera is a StandardViewCamera op now.)
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/render/renderer/FrameOrchestrator.hpp>            // FrameTickState
#include <lux/engine/render/renderer/RenderTargetRegistry.hpp>
#include <lux/engine/render/renderer/Renderer.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>            // Device/ResourceContext
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/renderer/FrameDriver.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>
#include <lux/engine/render/targets/OffscreenImagePool.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>

#include <imgui_impl_vulkan.h>

#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace lux::ui
{
    // ─────────────────────────────────────────────────────────────────────────
    //  TexResolverCtx — user_data for the Ex texture resolver callback
    // ─────────────────────────────────────────────────────────────────────────
    struct TexResolverCtx
    {
        lux::render::Renderer *renderer{nullptr};
        uint32_t frame_index{0};
        UIRenderServer::UIState *ui{nullptr};
        /// M5:此前持基类的私有 Impl —— 只为了 targets 寻址。改持注册表本身,
        /// 解析器要什么就拿什么。
        lux::render::RenderTargetRegistry *targets{nullptr};
        std::vector<std::unordered_map<uint64_t, VkDescriptorSet>> texture_ds_cache_per_frame;
    };

    static std::unordered_map<uint64_t, VkDescriptorSet> &getFrameTextureCache(TexResolverCtx &ctx)
    {
        if (ctx.texture_ds_cache_per_frame.empty())
            ctx.texture_ds_cache_per_frame.resize(1);

        const size_t slot = static_cast<size_t>(ctx.frame_index) % ctx.texture_ds_cache_per_frame.size();
        return ctx.texture_ds_cache_per_frame[slot];
    }

    static void clearTextureCacheSlot(
        ImGui_ImplVulkan_Renderer *renderer,
        std::unordered_map<uint64_t, VkDescriptorSet> &cache)
    {
        if (!renderer)
        {
            cache.clear();
            return;
        }

        for (const auto &[_, ds] : cache)
        {
            if (ds != VK_NULL_HANDLE)
                ImGui_ImplVulkan_RemoveTextureEx(renderer, ds);
        }
        cache.clear();
    }

    static void clearAllTextureCaches(ImGui_ImplVulkan_Renderer *renderer, TexResolverCtx &ctx)
    {
        for (auto &cache : ctx.texture_ds_cache_per_frame)
            clearTextureCacheSlot(renderer, cache);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  UIState — extra state that UIRenderServer stores alongside Impl
    // ─────────────────────────────────────────────────────────────────────────

    struct UIRenderServer::UIState
    {
        // (原 ui_target_by_key_ sentinel 侧表已消亡:sentinel 直接编码
        //  RenderTargetId,解析走 base targets_ + SAMPLED flag 不变量——
        //  SAMPLED 目标的 pool 静态类型必为 UIOffscreenImagePool,由
        //  uiMakeTargetPool 建立。)

        struct RetiredUIOffscreenPool
        {
            std::unique_ptr<UIOffscreenImagePool> ui_pool;
            uint64_t retire_frame{0};
        };
        std::vector<RetiredUIOffscreenPool> retired_ui_pools_;

        // ── ImGui 叠加(M4b:UILayer 直录,伪场景已消亡)──────────────
        // 原 imgui_scene_/imgui_view_/imgui_feature_ 伪 RenderScene 全链死:
        // 叠加不再经渲染图,tick 直接在 swapchain 图像上录
        // 屏障 + BeginRendering + RenderDrawDataEx + PRESENT 转换。
        ImGuiCommConfig imgui_cfg_{};   ///< 字体像素/格式(原经 feature 实例中转)
        ImGui_ImplVulkan_Renderer *imgui_vk_renderer_{nullptr};
        TexResolverCtx resolver_ctx_;

        // ── ImGui operation IDs (set during init) ─────────────────────────
        ImGuiOperationIds imgui_ops_{};

        // ── Draw data from SubmitImGuiDrawData handler ──────────────────────
        ImDrawDataSnapshot *pending_snapshot{nullptr};

        // ── 主窗合成链的形状 ────────────────────────────────────────────
        //
        // M4d:场景层不再另存一份。它就是主 Surface target 的 layers 里那个
        // SceneView 层 —— 唯一真相源。此前 swapchain_layer_ 除了记 scene/view,
        // 还自带一份 layout,于是"叠加在不在"这件事要在三处各算一遍。相位
        // 现由合成链按位置导出,那份 layout 连同整个结构一起没了。
        //
        // 剩下的只有一个开关:叠加层在不在链上。
        bool imgui_overlay_enabled_{true};

        /// Persistent storage for the ImGui color format so that the
        /// VulkanInitInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats
        /// pointer remains valid for the lifetime of the renderer.
        VkFormat imgui_color_format_{VK_FORMAT_B8G8R8A8_SRGB};

        // (overlay_layout_/overlay_layout_dirty_/recompileImGuiOverlayGraph
        //  已随伪场景消亡:相位由合成位置决定——场景层在下 → LOAD,
        //  否则 CLEAR;末了统一转 PRESENT。直录零重编。)

        /// M4c:副视口收编——每个 imgui 副视口 = 一个引擎 Surface target
        ///(PresentContext)+ 一个专属顶点环。键 = ViewportData*(快照的
        /// ViewportFrameEntry.vd 与三类事件都携带它)。取代 active_viewport_vds_。
        struct ViewportTarget
        {
            lux::render::RenderTargetId target{};
            void* ring{nullptr};   ///< ImGui_ImplVulkan_CreateRenderBuffersEx 句柄
            /// M4d:本条目自身就是该视口叠加层的 user 指针(unordered_map 结点
            /// 地址稳定)。回调靠这两个字段回到 UIState 并认出自己是哪个视口。
            UIState* owner{nullptr};
            void*    vd{nullptr};   ///< 键的副本 = ImGui ViewportData*
            //(resizing 已并入 target entry 的 rebuild_suspended —— 见
            // ResizeBegin 处的说明。)
        };
        std::unordered_map<void*, ViewportTarget> viewport_targets_;
        //(PendingViewportPresent/viewport_presents_ 已随 M4d 消亡:副视口的
        // acquire 与 present 由 FrameOrchestrator 的多 Surface 路径统一处理,
        // UI 只需给视口 target 挂一个叠加层。)
        uint32_t imgui_swapchain_image_count_{0};
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Texture resolver callback — runs per draw command inside RenderDrawDataEx
    // ─────────────────────────────────────────────────────────────────────────

    static VkDescriptorSet texResolverCallback(
        ImGui_ImplVulkan_Renderer *renderer,
        ImTextureID tex_id,
        void *user_data)
    {
        auto *ctx = static_cast<TexResolverCtx *>(user_data);

        if (isFontAtlasSentinel(tex_id))
        {
            VkDescriptorSet ds = ImGui_ImplVulkan_GetFontsTextureDescriptorSetEx(renderer);
            return ds;
        }

        if (isRenderTargetSentinel(tex_id))
        {
            const auto target = decodeRenderTarget(tex_id);
            if (ctx->targets)
            {
                if (auto* t = ctx->targets->tryGet(target);
                    t && t->pool && (t->flags & lux::render::kTargetFlagSampled))
                {
                    // SAMPLED 目标的 pool 静态类型必为 UIOffscreenImagePool
                    //(uiMakeTargetPool 建立的不变量)。
                    auto* ui_pool = static_cast<UIOffscreenImagePool*>(t->pool.get());
                    return ui_pool->imguiDescriptor(ctx->frame_index);
                }
            }
            return VK_NULL_HANDLE;
        }

        if (isTextureHandleSentinel(tex_id))
        {
            auto handle = decodeTextureHandle(tex_id);
            uint64_t key = (static_cast<uint64_t>(handle.index) << 16) | static_cast<uint64_t>(handle.gen);

            auto &frame_cache = getFrameTextureCache(*ctx);
            auto it = frame_cache.find(key);
            if (it != frame_cache.end())
                return it->second;

            auto *tex_res = ctx->renderer->renderContext()
                .globalRegistry()
                .find<lux::render::TextureResources>();

            if (tex_res)
            {
                lux::render::TextureHandle th{handle.index, handle.gen};
                VkImageView view = tex_res->imageView(th);
                VkSampler sampler = tex_res->sampler(th);
                if (view != VK_NULL_HANDLE && sampler != VK_NULL_HANDLE)
                {
                    VkDescriptorSet ds = ImGui_ImplVulkan_AddTextureEx(
                        renderer, sampler, view,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    frame_cache[key] = ds;
                    return ds;
                }
            }
            return VK_NULL_HANDLE;
        }

        // Not a sentinel — direct cast (stock ImGui behaviour)
        return (VkDescriptorSet)tex_id;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Dispatcher handlers (free functions, anonymous namespace)
    // ─────────────────────────────────────────────────────────────────────────

    /// M5:此前收一个基类私有 Impl 引用当"万能句柄"。改成显式收它真正要的
    /// 四样东西 —— 参数是多了,但这个函数从此不认识 Impl 的任何字段。
    [[nodiscard]] static lux::render::Expected<void>
    rebuildImGuiRendererForSwapchain(
        UIRenderServer::UIState &ui,
        lux::render::ResourceContext &rc,
        lux::render::Renderer *renderer,
        lux::render::RenderTargetRegistry &reg,
        uint32_t frames_in_flight,
        uint32_t image_count)
    {
        if (image_count == 0)
            return {};

        auto &cfg = ui.imgui_cfg_;

        if (ui.imgui_vk_renderer_ && ui.imgui_swapchain_image_count_ == image_count)
            return {};

        if (ui.imgui_vk_renderer_)
        {
            clearAllTextureCaches(ui.imgui_vk_renderer_, ui.resolver_ctx_);
            // M4c:renderer 重建把全部副视口 target(环 + vd + PresentContext)
            // 一并拆掉。每个 PresentContext::close() 显式证明 present queue
            // 已静默；副视口随后由 Created 事件重走收编。
            for (auto& [vd, vt] : ui.viewport_targets_)
            {
                if (vt.target.isValid())
                    if (auto* target = reg.tryGet(vt.target);
                        target && target->present)
                    {
                        auto closed = target->present->close();
                        if (!closed)
                        {
                            return lux::cxx::unexpected<lux::render::RenderError>(
                                closed.error()
                            );
                        }
                    }
                ImGui_ImplVulkan_DestroyRenderBuffersEx(ui.imgui_vk_renderer_, vt.ring);
                ImGui_ImplVulkan_DestroyViewportResourcesEx(ui.imgui_vk_renderer_, vd);
                if (vt.target.isValid())
                    reg.erase(vt.target);
            }
            ui.viewport_targets_.clear();
            ImGui_ImplVulkan_DestroyRendererEx(ui.imgui_vk_renderer_);
            ui.imgui_vk_renderer_ = nullptr;
        }

        VkPipelineRenderingCreateInfoKHR prc{};
        prc.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        prc.colorAttachmentCount = 1;
        prc.pColorAttachmentFormats = &ui.imgui_color_format_;

        ImGui_ImplVulkan_InitInfo init_info{};
        init_info.Instance = rc.instanceContext().instance();
        init_info.PhysicalDevice = rc.physicalDevice();
        init_info.Device = rc.logicalDevice();
        init_info.QueueFamily = rc.graphicsQueueFamilyIndex();
        init_info.Queue = rc.graphicsQueue();
        init_info.PipelineCache = VK_NULL_HANDLE;
        init_info.DescriptorPool = VK_NULL_HANDLE;
        init_info.DescriptorPoolSize = 1000;
        init_info.MinImageCount = image_count;
        init_info.ImageCount = image_count;
        init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.Allocator = rc.instanceContext().allocator();
        init_info.CheckVkResultFn = [](VkResult err)
        {
            // OUT_OF_DATE / SUBOPTIMAL / SURFACE_LOST are RECOVERABLE swapchain
            // transients (window resize, OS drag-drop surface churn). lux's
            // SwapchainProvider rebuilds on them (acquire catch-all + FrameDriver
            // present), and the ImGui renderer is recreated via the swapchain
            // rebuild callback — so they are NOT errors. Logging SURFACE_LOST
            // (-1000000000) here was alarming noise on every drag-drop.
            if (err == VK_SUCCESS
                || err == VK_SUBOPTIMAL_KHR
                || err == VK_ERROR_OUT_OF_DATE_KHR
                || err == VK_ERROR_SURFACE_LOST_KHR)
                return;
            std::fprintf(stderr, "[UIRenderServer] Vulkan error: VkResult = %d\n", err);
        };
        init_info.UseDynamicRendering = true;
        init_info.PipelineRenderingCreateInfo = prc;

        ui.imgui_vk_renderer_ = ImGui_ImplVulkan_CreateRendererEx(&init_info);
        ui.imgui_swapchain_image_count_ = image_count;

        if (ui.imgui_vk_renderer_ && cfg.font_pixels)
        {
            ImGui_ImplVulkan_CreateFontsTextureEx(
                ui.imgui_vk_renderer_, cfg.font_pixels, cfg.font_width, cfg.font_height);
        }

        ui.resolver_ctx_.renderer = renderer;
        ui.resolver_ctx_.ui = &ui;
        ui.resolver_ctx_.targets = &reg;
        ui.resolver_ctx_.texture_ds_cache_per_frame.resize(frames_in_flight);
        ImGui_ImplVulkan_SetTextureResolverEx(
            ui.imgui_vk_renderer_, &texResolverCallback, &ui.resolver_ctx_);

        // renderer 就位前建的 SAMPLED 目标此刻补挂 ImGui renderer
        //(uiMakeTargetPool 在 renderer 为空时挂的是 nullptr)。
        for (const auto key : reg.all().keys())
            if (auto* t = reg.tryGet(key);
                t && t->pool && (t->flags & lux::render::kTargetFlagSampled))
                static_cast<UIOffscreenImagePool*>(t->pool.get())
                    ->setImGuiRenderer(ui.imgui_vk_renderer_);
        return {};
    }

    namespace
    {
        using namespace lux::render;
        using Dispatcher = GeneralRenderServer::Dispatcher;
        using Ctx = Dispatcher::Ctx;

        // ── SubmitImGuiDrawData ──────────────────────────────────────────────

        void handleSubmitImGuiDrawData(Ctx &ctx, const SubmitImGuiDrawDataPayload &p)
        {
            auto *ui = static_cast<UIRenderServer::UIState *>(
                serverExtensionOf(ctx.user_state));
            if (!ui)
                return;

            if (p.attachment_index < ctx.program.attachments.size())
            {
                auto &att = ctx.program.attachments[p.attachment_index];
                if (att.type_id == kImGuiDrawDataAttachment)
                    ui->pending_snapshot = static_cast<ImDrawDataSnapshot *>(att.object);
            }
        }

        // (原 addUIView/removeUIView 命令面与共享 helpers 已消亡:UI 目标
        //  统一走 CreateOffscreenTarget(SAMPLED) + SetLayer,池型/退休由下面
        //  的扩展点接管;extent 越界防线在 base 层已有——池创建按请求值,
        //  多视口非法 extent 的守卫在 viewport 路径。)

        // ── Target 池扩展点(基类 makeTargetPool / retireTargetPool 经此
        //    把 SAMPLED 目标的池型与退休路由交给 UI 层)────────────────────

        std::unique_ptr<lux::render::OffscreenImagePool> uiMakeTargetPool(
            lux::render::RenderTargetRegistry &reg,
            const lux::render::RenderTargetLayout &layout,
            VkExtent2D extent, uint32_t target_flags)
        {
            auto *ui = static_cast<UIRenderServer::UIState *>(reg.userData());
            if (!ui || !(target_flags & lux::render::kTargetFlagSampled))
                return nullptr;   // 非 SAMPLED:基类池
            auto pool = std::make_unique<UIOffscreenImagePool>(
                reg.resourceContext(), layout, extent, reg.framesInFlight());
            pool->setImGuiRenderer(ui->imgui_vk_renderer_);
            return pool;
        }

        bool uiRetireTargetPool(
            lux::render::RenderTargetRegistry &reg,
            std::unique_ptr<lux::render::OffscreenImagePool> &pool,
            uint32_t target_flags, uint64_t retire_serial)
        {
            auto *ui = static_cast<UIRenderServer::UIState *>(reg.userData());
            if (!ui || !(target_flags & lux::render::kTargetFlagSampled))
                return false;     // 非 SAMPLED:基类 fence 水位延迟释放
            // SAMPLED 目标的池必为 UIOffscreenImagePool(uiMakeTargetPool /
            // handleAddUIView 建立的不变量)——转入 UI 侧退休列表,ImGui
            // 描述符在 renderer 存活期内释放。
            ui->retired_ui_pools_.push_back(
                {std::unique_ptr<UIOffscreenImagePool>(
                     static_cast<UIOffscreenImagePool *>(pool.release())),
                 retire_serial});
            return true;
        }

        // ── Pre-destroy-scene callback ──────────────────────────────────────

        void uiPreDestroyScene(void *extension, lux::render::RenderSceneId scene_id)
        {
            auto *ui = static_cast<UIRenderServer::UIState *>(extension);
            if (!ui || !ui->resolver_ctx_.targets)
                return;
            // (UI 目标的池在 base handleDestroyScene 级联摘层时经统一退休
            //  入口 retireTargetPool 路由——SAMPLED flag 把 UI 池转入
            //  retired_ui_pools_,描述符退休语义保持在 UI 侧;此处无需插手。)

            // 场景层引用了正在销毁的场景 → 摘层。视图不删:场景销毁会连它
            // 自己的视图一起清。
            //(M4d:基类 handleDestroyScene 级联摘层时已按 scene_id 清过主
            // Surface 的场景层;这里保留是为了服务端直呼 destroyScene 的路径。)
            if (auto *st = ui->resolver_ctx_.targets->surfaceTarget())
            {
                using Layer = lux::render::RenderTargetEntry::CompositeLayer;
                for (size_t i = 0; i < st->layers.size(); ++i)
                    if (st->layers[i].kind == Layer::EKind::SceneView &&
                        st->layers[i].scene_id == scene_id)
                    {
                        st->layers.erase(st->layers.begin() + static_cast<ptrdiff_t>(i));
                        break;
                    }
            }
        }

        // (handleUIRequestSwapchainScene 已消亡:RequestSwapchainScene 命令
        //  面零调用方——服务端直呼 setSwapchainScene 是唯一在用的上屏路径,
        //  M3 的 createSurfaceRenderTarget 命令面接棒。)
    } // anonymous namespace

    // ─────────────────────────────────────────────────────────────────────────
    //  ImGui ops register/unregister — called by kImGuiFeatureFactory
    // ─────────────────────────────────────────────────────────────────────────

    uint32_t imguiRegisterOpsFn(void *dispatcher, lux::render::TypeId *out_ops, uint32_t /*max_ops*/)
    {
        using namespace lux::render;
        auto &d = *static_cast<GeneralRenderServer::Dispatcher *>(dispatcher);
        out_ops[0] = d.allocateAndRegisterUnary<
            SubmitImGuiDrawDataPayload, &handleSubmitImGuiDrawData>(
            opcodes::CommandOp, imgui_ops::kSubmitDrawData);
        // (kAddUIView / kRemoveUIView 已消亡:UI 目标走 CreateOffscreenTarget
        //  (SAMPLED) + SetLayer 核心命令面。)
        return 1;
    }

    void imguiUnregisterOpsFn(void *dispatcher, const lux::render::TypeId *ops, uint32_t op_count)
    {
        using namespace lux::render;
        auto &d = *static_cast<GeneralRenderServer::Dispatcher *>(dispatcher);
        for (uint32_t i = 0; i < op_count; ++i)
            d.freeSlot(opcodes::CommandOp, ops[i]);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Construction / destruction
    // ─────────────────────────────────────────────────────────────────────────
    UIRenderServer::UIRenderServer(
        std::shared_ptr<Channel> frame_channel,
        std::shared_ptr<lux::render::RenderControlChannel<>> control_channel,
        std::shared_ptr<lux::render::RenderUploadChannel<>> upload_channel,
        std::shared_ptr<lux::render::RenderChannelSync> sync)
        : GeneralRenderServer(
              std::move(frame_channel),
              std::move(control_channel),
              std::move(upload_channel),
              std::move(sync)),
          ui_(std::make_unique<UIState>())
    {
        setExtension(ui_.get(), &uiPreDestroyScene);
        // 池的创建/退休扩展点随 target 一起归了 L4 的注册表。
        targets().setUserData(ui_.get());
        targets().setPoolCallbacks(&uiMakeTargetPool, &uiRetireTargetPool);
        // (ImGui + UIView handlers now registered dynamically via kImGuiFeatureFactory::register_ops_fn)
    }

    UIRenderServer::~UIRenderServer()
    {
        // Ensure no in-flight command still references ImGui descriptors/images.
        if (auto* dev = deviceContext())
            if (dev->logicalDevice().waitIdle() != VK_SUCCESS)
            {
                lux::render::renderFatal(
                    "UIRenderServer failed to wait for device idle during teardown"
                );
            }

        // M4c:两阶段销毁的在途账本先清(其 on_teardown 收尾捕获 renderer
        // 指针,必须在 renderer 销毁前跑;GPU 已 idle,直接执行)。
        flushPendingSurfaceReleases();
        // Destroy still-open secondary viewports(环 + vd + target entry)。
        // GPU is idle so this is safe.
        if (ui_->imgui_vk_renderer_)
        {
            for (auto& [vd, vt] : ui_->viewport_targets_)
            {
                ImGui_ImplVulkan_DestroyRenderBuffersEx(ui_->imgui_vk_renderer_, vt.ring);
                ImGui_ImplVulkan_DestroyViewportResourcesEx(ui_->imgui_vk_renderer_, vd);
                if (impl_ && vt.target.isValid())
                {
                    if (auto* target = targets().tryGet(vt.target);
                        target && target->present)
                    {
                        auto closed = target->present->close();
                        if (!closed)
                        {
                            lux::render::renderFatal(
                                "viewport PresentContext close failed during shutdown"
                            );
                        }
                    }
                    targets().erase(vt.target);
                }
            }
            ui_->viewport_targets_.clear();
        }

        // 场景销毁前先摘掉主窗场景层的视图(链是唯一真相源,从链上读)。
        clearSwapchainScene();

        if (ui_->imgui_vk_renderer_)
            clearAllTextureCaches(ui_->imgui_vk_renderer_, ui_->resolver_ctx_);

        // Destroy UIOffscreenImagePools BEFORE the ImGui renderer — they call
        // ImGui_ImplVulkan_RemoveTextureEx which requires a live renderer.
        // (GPU 已 idle:SAMPLED 目标直接删,池经基类虚析构在此销毁。)
        {
            const auto keys = targets().all().keys();   // 拷贝:循环内 erase
            for (const auto key : keys)
                if (auto* t = targets().tryGet(key);
                    t && (t->flags & lux::render::kTargetFlagSampled))
                    targets().erase(key);
        }
        ui_->retired_ui_pools_.clear();

        if (ui_->imgui_vk_renderer_)
        {
            ImGui_ImplVulkan_DestroyRendererEx(ui_->imgui_vk_renderer_);
            ui_->imgui_vk_renderer_ = nullptr;
        }
        setExtension(nullptr, nullptr);
    }

    ImGuiOperationIds UIRenderServer::imguiOps() const noexcept { return ui_->imgui_ops_; }

    // ─────────────────────────────────────────────────────────────────────────
    //  Swapchain scene — direct rendering to swapchain surface
    // ─────────────────────────────────────────────────────────────────────────

    // (recompileImGuiOverlayGraph 已随伪场景消亡:叠加直录,LOAD/CLEAR
    //  相位由合成位置在 tick 决定,PRESENT 转换由直录末屏障完成。)

    /// M4b:ImGui 叠加直录——伪场景/渲染图的三件事(布局屏障、
    /// BeginRendering 相位、末了终态转换)在此一体完成。
    ///
    /// M4d:相位不再由调用方手算。@p phase 由合成链按"我是第几层"给出,
    /// 终态从 @p binding 携带的**已相位化布局**里读 —— 建场景层时一处、
    /// 开关叠加时一处、录制时一处,三处各算一遍且必须一致的老账就此消掉。
    ///
    /// data 为空(本帧该窗口没有 draw data)时仍照常清屏并转终态:已经
    /// acquire 的图像必须被写过再呈现,否则呈现 UNDEFINED 布局 + acquire
    /// 信号量无人消费,下一次同槽 acquire 撞 "semaphore has pending operations"。
    static void recordImGuiOverlayLayer(
        VkCommandBuffer cmd,
        const lux::render::RenderTargetBinding &binding,
        const lux::render::LayerPhase &phase,
        ImGui_ImplVulkan_Renderer *renderer, ImDrawData *data,
        void *render_buffers)
    {
        const auto &slot_images = binding.slot(lux::render::TargetSlot::SCENE_COLOR);
        if (cmd == VK_NULL_HANDLE || !renderer ||
            slot_images.images.empty() || slot_images.views.empty())
            return;

        const VkImage     image  = slot_images.images.front();
        const VkImageView view   = slot_images.views.front();
        const VkExtent2D  extent = binding.extent;
        if (image == VK_NULL_HANDLE || view == VK_NULL_HANDLE)
            return;

        // 非首层 = 图像已有内容(场景层在下)→ LOAD 且自 COLOR_ATTACHMENT 接力。
        const bool composite_over_scene = !phase.is_first;

        // 终态由相位化布局给出:末层收尾到 target 终态(Surface 即 PRESENT_SRC),
        // 非末层保持 COLOR_ATTACHMENT 交棒给下一层。
        VkImageLayout final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (binding.layout)
        {
            const auto &desc =
                binding.layout->slots[static_cast<size_t>(lux::render::TargetSlot::SCENE_COLOR)];
            if (desc.has_value())
                final_layout = lux::render::toVkImageLayout(
                    desc->final_state
                );
        }

        auto barrier2 = [&](VkImageLayout oldL, VkImageLayout newL,
                            VkPipelineStageFlags2 srcS, VkAccessFlags2 srcA,
                            VkPipelineStageFlags2 dstS, VkAccessFlags2 dstA)
        {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = srcS; b.srcAccessMask = srcA;
            b.dstStageMask = dstS; b.dstAccessMask = dstA;
            b.oldLayout = oldL;    b.newLayout = newL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };

        // 1) 进附件布局:场景层在下 = COLOR_ATTACHMENT 接力(RAW 同步);
        //    首层 = acquire 后 UNDEFINED(内容可弃)。
        if (composite_over_scene)
            barrier2(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        else
            barrier2(VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        // 2) BeginRendering:相位按合成位置(LOAD 叠加 / CLEAR 首层)。
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView   = view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp      = composite_over_scene ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                 : VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.f, 0.f, 0.f, 1.f}};

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea           = {{0, 0}, extent};
        ri.layerCount           = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments    = &color;
        vkCmdBeginRendering(cmd, &ri);

        if (data && data->Valid && data->TotalIdxCount > 0)
            ImGui_ImplVulkan_RenderDrawDataWithBuffersEx(renderer, data, cmd, render_buffers);

        vkCmdEndRendering(cmd);

        // 3) 末了转终态(present 的可见性由 submit 的 present sem 保证)。
        //    非末层时终态就是 COLOR_ATTACHMENT —— 无须转换,下一层自带入层
        //    屏障(场景层的渲染图、叠加层的上面那条),这里不重复发。
        if (final_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            barrier2(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, final_layout,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);
    }

    // ── 叠加层的两个录制回调(挂进 target 的合成链)─────────────────────
    //
    // 回调签名只给 (user, cmd, binding, phase) —— 每帧变化的 draw data 不能
    // 存进层里,得在录制时现取。这正是叠加"是一层"该有的样子:层描述**画什么**,
    // 不缓存**这帧画什么**。

    /// 主窗叠加层。user = UIState*。
    static void mainOverlayRecordFn(void *user, VkCommandBuffer cmd,
                                    const lux::render::RenderTargetBinding &binding,
                                    const lux::render::LayerPhase &phase)
    {
        auto *ui = static_cast<UIRenderServer::UIState *>(user);
        if (!ui || !ui->imgui_vk_renderer_)
            return;
        recordImGuiOverlayLayer(
            cmd, binding, phase, ui->imgui_vk_renderer_,
            ui->pending_snapshot ? &ui->pending_snapshot->drawData() : nullptr,
            /*render_buffers=*/nullptr);
    }

    /// 副视口叠加层。user = &UIState::ViewportTarget(unordered_map 的结点
    /// 地址稳定,可长期持有)。本视口的 draw data 每帧按 vd 在快照里匹配。
    static void viewportOverlayRecordFn(void *user, VkCommandBuffer cmd,
                                        const lux::render::RenderTargetBinding &binding,
                                        const lux::render::LayerPhase &phase)
    {
        auto *vt = static_cast<UIRenderServer::UIState::ViewportTarget *>(user);
        if (!vt || !vt->owner || !vt->owner->imgui_vk_renderer_)
            return;
        auto *ui = vt->owner;

        ImDrawData *data = nullptr;
        if (ui->pending_snapshot)
        {
            const auto &frames = ui->pending_snapshot->viewportFrames();
            for (int i = 0; i < frames.Size; ++i)
                if (frames[i].vd == vt->vd)
                {
                    data = const_cast<ImDrawData *>(&frames[i].draw_data);
                    break;
                }
        }
        // data 为空照常清屏 —— 见 recordImGuiOverlayLayer 的说明。
        recordImGuiOverlayLayer(cmd, binding, phase, ui->imgui_vk_renderer_,
                                data, vt->ring);
    }

    /// 按 ImGui ViewportData* 取该副视口的 target entry(没有则 nullptr)。
    static lux::render::RenderTargetEntry *viewportEntry(
        lux::render::RenderTargetRegistry &reg,
        UIRenderServer::UIState *ui, void *vd)
    {
        if (!ui)
            return nullptr;
        auto it = ui->viewport_targets_.find(vd);
        if (it == ui->viewport_targets_.end() || !it->second.target.isValid())
            return nullptr;
        return reg.tryGet(it->second.target);
    }

    /// 按**本帧快照**同步各副视口的合成层:快照里有这个 vd 的条目才挂层。
    ///
    /// 为什么必须每帧同步,而不是建视口时挂上就不管:
    ///
    /// 合成链上的层决定了编排 acquire 哪些呈现面(FrameOrchestrator 的唯一门控
    /// 就是 layers.empty())。而副视口是否该呈现是**每帧**的事 ——
    /// ImGuiDrawDataSnapshot 会把 IsMinimized(或 RendererUserData 为空)的副视口
    /// **整个跳过**,不进 viewport_frames_。
    ///
    /// 层若常驻,这类副视口就会照常被 acquire,而录制回调找不到 draw data 只能
    /// 清屏 —— 于是窗口内容变黑。4b 把逐视口循环从"draw data 驱动"改成
    /// "target 驱动"时漏了这一层:旧代码遍历 frames,这类副视口根本不被访问,
    /// 不 acquire 也不 present,窗口自然保留上一帧内容。本函数把那个语义还原成
    /// 链上的显式状态:**层在 = 本帧要画**,而不是"曾经建过"。
    ///
    /// 判据与旧代码逐字一致:只看**快照里有没有这个 vd 的条目**,不看条目里的
    /// draw_data 是否 Valid —— 后者旧代码同样会清屏(loadOp CLEAR + 不发绘制),
    /// 那不是回归,不要顺手改掉。
    static void syncViewportLayers(lux::render::RenderTargetRegistry &reg,
                                   UIRenderServer::UIState *ui)
    {
        using Layer = lux::render::RenderTargetEntry::CompositeLayer;
        if (!ui)
            return;

        for (auto &[vd, vt] : ui->viewport_targets_)
        {
            if (!vt.target.isValid())
                continue;                       // create 失败的占位
            auto *t = reg.tryGet(vt.target);
            if (!t)
                continue;

            bool present_this_frame = false;
            if (ui->pending_snapshot)
            {
                const auto &frames = ui->pending_snapshot->viewportFrames();
                for (int i = 0; i < frames.Size; ++i)
                    if (frames[i].vd == vd)
                    {
                        present_this_frame = true;
                        break;
                    }
            }

            t->layers.clear();
            if (present_this_frame)
                t->layers.push_back(
                    Layer::customRecord(&viewportOverlayRecordFn, &vt));
        }
    }

    /// 主 Surface 的合成链里那个场景层(没有则 nullptr)。
    static lux::render::RenderTargetEntry::CompositeLayer *findSceneLayer(
        lux::render::RenderTargetRegistry &targets)
    {
        using Layer = lux::render::RenderTargetEntry::CompositeLayer;
        auto *t = targets.surfaceTarget();
        if (!t)
            return nullptr;
        for (auto &l : t->layers)
            if (l.kind == Layer::EKind::SceneView)
                return &l;
        return nullptr;
    }

    /// 把主窗合成链同步成 [场景层?][叠加层?]。
    ///
    /// 顺序即语义:场景在下、叠加在上。谁 CLEAR 谁 LOAD、谁转 PRESENT,全部
    /// 由 FrameOrchestrator 按位置导出 —— 这个函数只决定"链上有哪几层"。
    static void syncSurfaceChain(lux::render::RenderTargetRegistry &targets,
                                 UIRenderServer::UIState *ui)
    {
        using Layer = lux::render::RenderTargetEntry::CompositeLayer;
        auto *t = targets.surfaceTarget();
        if (!t || !ui)
            return;

        std::optional<Layer> scene;
        if (auto *l = findSceneLayer(targets))
            scene = *l;

        t->layers.clear();
        if (scene.has_value())
            t->layers.push_back(*scene);
        if (ui->imgui_overlay_enabled_)
            t->layers.push_back(Layer::customRecord(&mainOverlayRecordFn, ui));
    }

    // (服务端直呼 addUIView / removeUIView 已消亡:UI 目标统一走
    //  CreateOffscreenTarget(SAMPLED) + SetLayer 命令面。)

    lux::render::ViewHandle UIRenderServer::setSwapchainScene(lux::render::RenderSceneId scene_id)
    {
        const lux::render::ViewHandle kInvalid{};

        if (!swapchainProvider())
            return kInvalid;  // no swapchain attached

        if (findSceneLayer(targets()))
            return kInvalid;  // already set — call clearSwapchainScene() first

        auto *scene = renderer().getScene(scene_id);
        if (!scene)
            return kInvalid;

        auto *sc = swapchainProvider();
        const auto sc_extent = sc->extent();

        // Create an internal view for swapchain rendering
        lux::render::ViewCreateInfo ci{
            .initial_extent = {sc_extent.width, sc_extent.height},
            .debug_name = "SwapchainSceneView",
        };
        lux::render::ViewHandle view_id = scene->addView(ci);
        auto *view = scene->getView(view_id);
        if (!view)
            return kInvalid;

        // M4d:这里原本手写一份 scene_layout,并按"叠加开没开"选 final_layout
        //(COLOR_ATTACHMENT 交棒 / PRESENT 收尾)。那是**相位**,不是布局 ——
        // 现在由合成链按场景层在链中的位置导出:叠加在上就交棒,场景层独占
        // 全链就收尾到 target 的终态(swapchain 的 PRESENT_SRC)。
        // 于是这一整块连同它与另两处的手工同步一起消失。

        // Register with base class swapchain binding (unbind any previous first)
        if (hasSwapchainBinding())
            unbindSwapchain();

        // 交给基类的是 **swapchain 自己的终态布局**(final = PRESENT_SRC)——
        // target 的布局本就该是它的终态,链上每一层实际用的布局由相位导出。
        // 安全性依据:M2c 之后 final_layout/is_presentable 是运行时参数,不进
        // 编译模板;而场景层恒为首层,preserve_content 两种相位下都是 false。
        // 所以模板只编一次,开关叠加零重编。
        auto bind_result = bindSwapchain(scene_id, view_id, sc->layout());
        if (!bind_result)
        {
            (void)scene->removeView(view_id);   // 刚 addView 的,必可摘
            return kInvalid;
        }

        // 基类已把场景层放上链(且清空了旧链);这里只补叠加层。
        syncSurfaceChain(targets(), ui_.get());

        return view_id;
    }

    void UIRenderServer::clearSwapchainScene()
    {
        using Layer = lux::render::RenderTargetEntry::CompositeLayer;

        auto *t = targets().surfaceTarget();
        auto *scene_layer = findSceneLayer(targets());
        if (!t || !scene_layer)
            return;

        const auto scene_id = scene_layer->scene_id;
        const auto view_id  = scene_layer->view_id;

        // 先摘层再删视图 —— 反过来的话中间态里链上挂着一个已死的视图。
        for (size_t i = 0; i < t->layers.size(); ++i)
            if (t->layers[i].kind == Layer::EKind::SceneView)
            {
                t->layers.erase(t->layers.begin() + static_cast<ptrdiff_t>(i));
                break;
            }

        if (view_id.isValid())
            if (auto *scene = renderer().getScene(scene_id))
                (void)scene->removeView(view_id);   // 重复摘无害,swapchain 链不在乎
    }

    void UIRenderServer::setImGuiOverlayEnabled(bool enabled)
    {
        if (ui_->imgui_overlay_enabled_ == enabled)
            return;

        ui_->imgui_overlay_enabled_ = enabled;

        // M4d:这里原本手改场景层布局的 final_layout/is_presentable —— 与
        // setSwapchainScene 里那份、以及录制时那份必须三处一致。现在只改
        // **链的形状**:叠加层上链或下链,相位随之重算。零重编依旧。
        syncSurfaceChain(targets(), ui_.get());
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  init — eager ImGui scene/feature creation
    // ─────────────────────────────────────────────────────────────────────────

    lux::render::Expected<void> UIRenderServer::init(
        lux::render::ServerConfig config,
        const ImGuiCommConfig &imgui_config)
    {
        // 1. Base init — Vulkan stack, dispatcher, etc.
        auto r = GeneralRenderServer::init(std::move(config));
        if (!r)
            return r;

        // (1b. 原 RequestSwapchainScene 覆盖注册已随命令面消亡。)

        // 2. Register the ImGui feature factory(op 注册是类型级的——
        //    SubmitImGuiDrawData handler 由 register_ops_fn 绑定,无需实例)。
        auto imgui_reg = addFeatureFactory(kImGuiFeatureFactory);
        ui_->imgui_ops_ = ImGuiOperationIds::fromOps(imgui_reg.ops, imgui_reg.op_count);

        // 3.(M4b)伪场景已消亡:叠加直录,配置直接存 UIState;renderer
        //    创建仍推迟到 swapchain attach(要 imageCount)。
        ui_->imgui_cfg_ = imgui_config;
        switch (imgui_config.color_format)
        {
        default:
            ui_->imgui_color_format_ = VK_FORMAT_B8G8R8A8_SRGB;
            break;
        }

        return {};
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  attachToWindow — base attachToWindow + eager ImGui swapchain view
    // ─────────────────────────────────────────────────────────────────────────

    lux::render::Expected<void> UIRenderServer::attachToWindow(lux::window::LuxWindow &window)
    {
        auto r = GeneralRenderServer::attachToWindow(window);
        if (!r)
            return r;

        if (swapchainProvider())
        {
            const uint32_t image_count = swapchainProvider()->imageCount();
            auto rebuilt = rebuildImGuiRendererForSwapchain(
                *ui_,
                resourceContext(),
                &renderer(),
                targets(),
                framesInFlight(),
                image_count
            );
            if (!rebuilt)
                return lux::cxx::unexpected<lux::render::RenderError>(rebuilt.error());

            swapchainProvider()->setRebuildCallback(
                [this]() -> lux::render::Expected<void>
                {
                    auto *sc = swapchainProvider();
                    if (!sc)
                        return {};
                    // (叠加直录零重编——原 recompileImGuiOverlayGraph 已消亡。)
                    return rebuildImGuiRendererForSwapchain(
                        *ui_,
                        resourceContext(),
                        &renderer(),
                        targets(),
                        framesInFlight(),
                        sc->imageCount()
                    );
                }
            );
        }

        // (M4b:原"伪场景 ImGuiSwapchainView 急建"已消亡——直录不需要视图。)

        return {};
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  tick — override with ImGui support
    // ─────────────────────────────────────────────────────────────────────────

    bool UIRenderServer::tick()
    {
        assert(ui_ && "UIState should be initialized in the constructor");
        assert(ui_->imgui_vk_renderer_ && "ImGui VkRenderer should be created at attach");

        // 阶段 0:帧戳 + 排水 + 冲刷延迟回复。走基类,不再自己抄一遍。
        // 排空之后、开帧之前这个缝,是 UI 唯一能看到"本帧要画什么"的时刻 ——
        // 下面两个链同步必须落在这里。
        if (!drainTick())
            return false;

        // (可渲染性判定已归编排:beginRenderFrame 返回 NoTarget 即无事可做。
        //  判据也随之精确了一档 —— 此前是"有 swapchain 就算能渲",现在是
        //  "主 Surface 链上确实有层" :叠加关掉且没绑场景层时,不再空转
        //  acquire 一张谁也不写的图像。)

        // (原"view.resize_pending 驱动的池随动 resize 循环"已消亡:改尺寸
        //  统一走 ResizeTarget 直达图像池(UI 池经虚 resize 同路),视图
        //  渲染尺寸由 binding 派生。M2c View 瘦身。)

        // ── M4c 副视口收编(帧前段):事件消费 + 重建扫描。必须先于
        //    beginFrame——重建要 waitAllFences,帧中调用会死锁(当前槽
        //    fence 已 reset);快照在 runTick 排水期已就位,事件天然先于
        //    本帧可用。
        if (ui_->pending_snapshot && ui_->imgui_vk_renderer_ &&
            !ui_->pending_snapshot->viewportEvents().empty())
        {
            using Entry = lux::render::RenderTargetEntry;
            // 尺寸护栏:多视口弹窗可在尺寸承诺前捕获 ImGuiViewport::Size,
            // 垃圾/零尺寸不得流进任何 create-info。provider 会按 surface
            // caps 钳制(Win32 下 min==max==currentExtent,即真实窗口
            // 尺寸),故 Created 以 1x1 占位即可自愈;Resized 直接丢弃——
            // ImGui 稍后必补发有效 Resized。
            static constexpr int kMaxViewportExtent = 16384;   // ~Vulkan maxImageDimension2D
            for (const auto& evt : ui_->pending_snapshot->viewportEvents())
            {
                const bool bad_extent =
                    (evt.width  <= 0 || evt.height <= 0 ||
                     evt.width  > kMaxViewportExtent || evt.height > kMaxViewportExtent);

                if (evt.type == ImGui_ImplVulkan_ViewportEvent::Created)
                {
                    const VkExtent2D ext = bad_extent
                        ? VkExtent2D{1u, 1u}
                        : VkExtent2D{static_cast<uint32_t>(evt.width),
                                     static_cast<uint32_t>(evt.height)};
                    // Created 自带 UI 线程建好的 VkSurfaceKHR(fork Ex 路径
                    // 所有权随事件移交)→ 收编为引擎 Surface target,与主窗
                    // 同构;副窗关 vsync,免多窗 FIFO 串行等待拖慢主循环。
                    auto pc = lux::render::PresentContext::create(
                        resourceContext(),
                        lux::render::RenderSurface::adopt(
                            evt.surface, ext,
                            resourceContext().instanceContext().instance()),
                        ext, /*enable_vsync=*/false,
                        /*enable_present_scaling=*/true);
                    if (!pc)
                    {
                        reportError(pc.error());
                        // create 失败已内部销毁 surface。仍登记 vd(无效
                        // target 占位):Destroyed 统一走 vd 收尾,不泄漏。
                        ui_->viewport_targets_[evt.viewport_data] =
                            UIState::ViewportTarget{};
                        continue;
                    }
                    Entry e{};
                    e.kind    = Entry::EKind::Surface;
                    e.layout  = (*pc)->provider()->layout();
                    e.present = std::move(*pc);
                    const auto tid = targets().insert(std::move(e));

                    // M4d:副视口 = 一个 Surface target + 一个叠加层。挂上层之后,
                    // 它的 acquire / 录制 / present 全部由帧编排的多 Surface
                    // 路径接管——UI 的 tick 里那段逐视口手工 acquire、注信号量、
                    // 攒 present 列表的代码随之消失。
                    //
                    // **这里不挂层。** 层的在与不在表达"本帧要不要画",由
                    // syncViewportLayers 按每帧快照决定(理由见那里)。建视口时
                    // 就常驻挂上,会让 IsMinimized 的副视口照样被 acquire 然后
                    // 清屏 —— 那正是拖拽越界时窗口变黑的根因。
                    //
                    // user 指向 map 结点自身(unordered_map 结点地址稳定)。
                    auto& vt = ui_->viewport_targets_[evt.viewport_data];
                    vt = UIState::ViewportTarget{
                        tid, ImGui_ImplVulkan_CreateRenderBuffersEx(ui_->imgui_vk_renderer_)};
                    vt.owner = ui_.get();
                    vt.vd    = evt.viewport_data;
                }
                else if (evt.type == ImGui_ImplVulkan_ViewportEvent::ResizeBegin)
                {
                    // 开括号:挂起本视口重建,直到 ResizeEnd。
                    // M4d:括号状态直接记在 target entry 上(rebuild_suspended)——
                    // 重建扫描已归编排统一执行,它只认 entry;再在 UI 侧留一份
                    // resizing 就是两份记录必须同步的老毛病。
                    if (auto* t = viewportEntry(targets(), ui_.get(), evt.viewport_data))
                        t->rebuild_suspended = true;
                }
                else if (evt.type == ImGui_ImplVulkan_ViewportEvent::ResizeEnd)
                {
                    // 闭括号:尺寸已定(ImGui 停止改动),标记重建——帧前段的
                    // 重建扫描会对已稳定的窗口尺寸重建一次(createSwapchain 现查
                    // caps 取 currentExtent),竞速窗口闭合。
                    if (auto* t = viewportEntry(targets(), ui_.get(), evt.viewport_data))
                    {
                        t->rebuild_suspended = false;
                        if (t->present)
                            t->present->provider()->markNeedsRebuild();
                    }
                }
                else if (evt.type == ImGui_ImplVulkan_ViewportEvent::Resized)
                {
                    if (bad_extent)
                        continue;
                    auto* t = viewportEntry(targets(), ui_.get(), evt.viewport_data);
                    if (!t || !t->present)
                        continue;
                    if (t->rebuild_suspended)
                        continue;   // 括号内:忽略中间尺寸,ResizeEnd 统一重建
                    t->present->provider()->requestResize(
                        {static_cast<uint32_t>(evt.width),
                         static_cast<uint32_t>(evt.height)});
                }
                else if (evt.type == ImGui_ImplVulkan_ViewportEvent::Destroyed)
                {
                    auto it = ui_->viewport_targets_.find(evt.viewport_data);
                    if (it == ui_->viewport_targets_.end())
                        continue;
                    // 两阶段销毁:受理即停呈现(entry 除名),PresentContext
                    // + 顶点环 + vd 随在途账本等 fence 水位一并拆
                    // (request_id=0 → 内部释放不发回执)。
                    const auto tid = it->second.target;
                    std::unique_ptr<lux::render::PresentContext> pc;
                    if (tid.isValid())
                        if (auto* t = targets().tryGet(tid); t)
                            pc = std::move(t->present);
                    auto teardown =
                        [renderer = ui_->imgui_vk_renderer_,
                         ring = it->second.ring, vd = evt.viewport_data]()
                    {
                        ImGui_ImplVulkan_DestroyRenderBuffersEx(renderer, ring);
                        ImGui_ImplVulkan_DestroyViewportResourcesEx(renderer, vd);
                    };
                    if (tid.isValid())
                        targets().erase(tid);
                    ui_->viewport_targets_.erase(it);
                    deferSurfaceRelease(tid, std::move(pc), std::move(teardown));
                }
            }
        }
        // 主窗合成链每 tick 自愈一次。必须在这里、而不是只在 setSwapchainScene /
        // setImGuiOverlayEnabled 里同步 —— 主 Surface target 有三条出生路径
        // (UI 直呼、命令面 BindSwapchain、命令面建面),且 bindSwapchainInternal
        // 会 clear() 整条链。只要叠加是"开着的",它就该在链上,无论 target 是
        // 谁在什么时候造的。纯 ImGui 主窗(编辑器的实际形态:3D 全在离屏面板,
        // 主窗只有界面)因此不依赖任何场景层就能上屏。
        syncSurfaceChain(targets(), ui_.get());

        // 副视口的链按**本帧快照**同步:快照里有这个 vd 的条目才挂层。
        // 必须在 beginRenderTick 之前 —— 编排一进去就按 layers 决定 acquire
        // 哪些呈现面,那时再改已经晚了。
        syncViewportLayers(targets(), ui_.get());

        // ── 阶段 1:开帧 ──────────────────────────────────────────────────
        //
        // 副视口的重建扫描、acquire、信号量注入,原先都在这个函数里手写一遍。
        // 它们现在是编排对**所有** Surface target 一视同仁的处理:主窗由
        // FrameDriver 走既有路径,其余的在 beginRenderFrame 里补齐。
        //
        // M5:改走基类的 protected 三阶段,不再自己持 orchestrator/FrameDriver
        // 并手动排水、开帧、退休记账。
        lux::render::FrameTickState fs{};
        const auto start = beginRenderTick(fs);
        if (start == ETickStage::NoTarget)
        {
            if (ui_->imgui_vk_renderer_)
                clearAllTextureCaches(ui_->imgui_vk_renderer_, ui_->resolver_ctx_);
            // 派生 tick 也必须保持基类的两阶段 Surface 销毁契约：最后一个
            // target 已除名后不会再有 Ready 帧替我们发送 TargetReleased。
            return stepPendingSurfaceReleases();
        }
        if (start == ETickStage::Skipped)
            return stepPendingSurfaceReleases();
        if (start == ETickStage::Failed)
            return false;

        // ── UI 的帧前准备:采样前补描述符 + 换纹理缓存槽 ────────────────
        //
        // 叠加要采样 SAMPLED 目标的色图,描述符必须在录制前就位。放在这里
        // (而不是原先夹在离屏循环之后)是因为池只在排水期增删,渲染阶段
        // 不再变化 —— 提前到渲染之前反而更早、更稳。
        for (const auto key : targets().all().keys())
            if (auto *t = targets().tryGet(key);
                t && t->pool && (t->flags & lux::render::kTargetFlagSampled))
                static_cast<UIOffscreenImagePool *>(t->pool.get())->ensureImGuiDescriptors();

        ui_->resolver_ctx_.frame_index = currentStamp().slotIndex();
        clearTextureCacheSlot(ui_->imgui_vk_renderer_, getFrameTextureCache(ui_->resolver_ctx_));

        // ── 阶段 2:渲染 ─────────────────────────────────────────────────
        //
        // 原先这里是四段手写代码:离屏循环、swapchain 场景层、ImGui 叠加直录、
        // 副视口逐个 acquire+录制。它们的共同点是"往某个 target 的合成链上
        // 依次画东西" —— 现在四段都成了链上的层,由编排统一走:
        //
        //   离屏 target   [场景层…]                    ← 与基类同
        //   主 Surface    [场景层?, 叠加层?]           ← syncSurfaceChain 维护
        //   各副视口      [叠加层]                     ← 视口 Created 时挂上
        //
        // 相位(CLEAR/LOAD、终态 PRESENT/COLOR_ATTACHMENT)由层在链中的位置
        // 导出,不再有任何一处手算。
        //
        // 一处行为变更:离屏层此前恒传 cross_view_index=0,现随基类走批计数。
        // 录制器的该参数管的是**跨视图共享的 imported 资源**(它们确实停在上
        // 一视图留下的终态),不是各 target 自己的图像 —— 后者由
        // first_view_barriers 处理,与本参数无关。同一场景渲进多个离屏 target
        // (编辑器多视口)正是它要覆盖的情形。
        renderRenderTick(fs);

        // ── 阶段 3:结帧 ─────────────────────────────────────────────────
        //
        // 提交(含各副视口折入的 acquire/present 信号量)、逐 Surface 呈现、
        // 目标池老化、Surface 拆除步进、异步回读、收尾记账 —— 全在里面。
        if (!endRenderTick(fs))
            return false;

        // UI 自己那份退休池(SAMPLED 目标的池带 ImGui 描述符,退休语义在 UI
        // 侧)仍需按同一水位老化。
        const uint64_t gpu_completed = gpuCompletedSerial();
        const uint64_t serial        = currentStamp().serial;

        auto retired_it = ui_->retired_ui_pools_.begin();
        while (retired_it != ui_->retired_ui_pools_.end())
        {
            if (!retired_it->ui_pool)
            {
                retired_it = ui_->retired_ui_pools_.erase(retired_it);
                continue;
            }

            retired_it->ui_pool->collectRetired(serial, gpu_completed);
            if (retired_it->retire_frame == 0)
                retired_it->retire_frame = serial;

            const bool aged_out = (retired_it->retire_frame <= gpu_completed);
            if (aged_out)
                retired_it = ui_->retired_ui_pools_.erase(retired_it);
            else
                ++retired_it;
        }

        //(Surface 拆除步进、异步回读、收尾记账已随 endRenderTick 归位 ——
        // 此前 UI 整份复制 tick,这三件必须在这里各抄一遍才不漏。)

        ui_->pending_snapshot = nullptr;
        return true;
    }

} // namespace lux::ui
