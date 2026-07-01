#include <lux/engine/ui/UIRenderServer.hpp>
#include <lux/engine/ui/ImGuiFeature.hpp>
#include <lux/engine/ui/ImGuiCommConfig.hpp>
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>
#include <lux/engine/ui/UIOffscreenImagePool.hpp>
#include <lux/engine/ui/ImGuiDrawDataSnapshot.hpp>

#include <lux/engine/render/comm/server/RenderServerImpl.hpp>
// (InitialViewCamera.hpp retired — initial camera is a StandardViewCamera op now.)
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/comm/RenderCommTypes.hpp>
#include <lux/engine/render/comm/RenderTickPipeline.hpp>
#include <lux/engine/render/renderer/Renderer.hpp>
#include <lux/engine/render/renderer/FrameDriver.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>
#include <lux/engine/render/targets/OffscreenImagePool.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>

#include <imgui_impl_vulkan.h>

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace lux::ui
{
    // Owner token for deferred viewport Vulkan resource cleanup.
    static constexpr lux::render::FrameRetireScheduler::OwnerToken kViewportRetireOwner = 0xFFFF'FFFF'DEAD'C0DEull;

    // ─────────────────────────────────────────────────────────────────────────
    //  TexResolverCtx — user_data for the Ex texture resolver callback
    // ─────────────────────────────────────────────────────────────────────────
    struct TexResolverCtx
    {
        lux::render::Renderer *renderer{nullptr};
        uint32_t frame_index{0};
        UIRenderServer::UIState *ui{nullptr};
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
        using SceneViewKey = uint64_t;

        // Keyed on the SLOT INDEX of scene + view (generation dropped): this is a
        // local lookup key for ui_offscreen_views_, and the ImTextureID sentinel
        // path (decodeSceneView) only carries indices anyway.
        static SceneViewKey makeSceneViewKey(
            lux::render::RenderSceneId scene_id, uint32_t view_index) noexcept
        {
            return (static_cast<SceneViewKey>(scene_id.index) << 32) | static_cast<SceneViewKey>(view_index);
        }

        // ── UI offscreen views (created by handleAddUIView) ─────────────────
        struct UIOffscreenViewEntry
        {
            lux::render::RenderSceneId scene_id;
            lux::render::ViewHandle view_id;
            std::unique_ptr<UIOffscreenImagePool> ui_pool;
            lux::render::RenderTargetLayout layout;
        };
        std::vector<UIOffscreenViewEntry> ui_offscreen_views_;
        std::unordered_map<SceneViewKey, size_t> scene_view_index_;

        struct RetiredUIOffscreenPool
        {
            std::unique_ptr<UIOffscreenImagePool> ui_pool;
            uint64_t retire_frame{0};
        };
        std::vector<RetiredUIOffscreenPool> retired_ui_pools_;

        // ── Eagerly-created ImGui overlay (set once in init()) ──────────────
        lux::render::RenderSceneId imgui_scene_id_{};
        lux::render::RenderScene *imgui_scene_{nullptr};
        lux::render::ViewHandle imgui_view_id_{};
        lux::render::View *imgui_view_{nullptr};
        ImGuiFeature *imgui_feature_{nullptr};
        ImGui_ImplVulkan_Renderer *imgui_vk_renderer_{nullptr};
        TexResolverCtx resolver_ctx_;

        // ── ImGui operation IDs (set during init) ─────────────────────────
        ImGuiOperationIds imgui_ops_{};

        // ── Draw data from SubmitImGuiDrawData handler ──────────────────────
        ImDrawDataSnapshot *pending_snapshot{nullptr};

        // ── Swapchain scene layer (set via setSwapchainScene) ───────────────
        struct SwapchainLayer
        {
            lux::render::RenderSceneId scene_id;
            lux::render::ViewHandle view_id{};
            lux::render::RenderTargetLayout layout;  ///< final_layout = COLOR_ATTACHMENT_OPTIMAL
        };
        std::optional<SwapchainLayer> swapchain_layer_;
        bool imgui_overlay_enabled_{true};

        /// Persistent storage for the ImGui color format so that the
        /// VulkanInitInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats
        /// pointer remains valid for the lifetime of the renderer.
        VkFormat imgui_color_format_{VK_FORMAT_B8G8R8A8_SRGB};

        /// Layout used for ImGui overlay rendering.  When a swapchain scene
        /// layer is active this is compiled with initial_layout=COLOR_ATTACHMENT_OPTIMAL
        /// and preserve_content=true so the overlay LOADs the scene content.
        lux::render::RenderTargetLayout overlay_layout_;
        bool overlay_layout_dirty_{false};  ///< requires ImGui graph recompile

        /// Tracks render-thread-owned viewport VDs so still-open secondary
        /// viewports can be cleaned up at shutdown.
        std::unordered_set<void*> active_viewport_vds_;
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

        if (isSceneViewSentinel(tex_id))
        {
            auto [scene_id, view_id] = decodeSceneView(tex_id);
            if (ctx->ui)
            {
                const auto key = UIRenderServer::UIState::makeSceneViewKey(scene_id, view_id);
                auto it = ctx->ui->scene_view_index_.find(key);
                if (it != ctx->ui->scene_view_index_.end())
                {
                    const size_t idx = it->second;
                    if (idx < ctx->ui->ui_offscreen_views_.size())
                    {
                        auto &e = ctx->ui->ui_offscreen_views_[idx];
                        return e.ui_pool->imguiDescriptor(ctx->frame_index);
                    }
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

    // Forward declaration — defined after the anonymous namespace.
    static void recompileImGuiOverlayGraph(UIRenderServer::UIState &ui, lux::render::SwapchainProvider *sc);

    static void rebuildImGuiRendererForSwapchain(
        UIRenderServer::UIState &ui,
        lux::render::GeneralRenderServer::Impl &im,
        uint32_t image_count)
    {
        if (!ui.imgui_feature_ || image_count == 0)
            return;

        auto &rc = *im.res_ctx_;
        auto &cfg = ui.imgui_feature_->initConfig();

        if (ui.imgui_vk_renderer_ && ui.imgui_swapchain_image_count_ == image_count)
            return;

        if (ui.imgui_vk_renderer_)
        {
            clearAllTextureCaches(ui.imgui_vk_renderer_, ui.resolver_ctx_);
            for (void* vd : ui.active_viewport_vds_)
                ImGui_ImplVulkan_DestroyViewportResourcesEx(ui.imgui_vk_renderer_, vd);
            ui.active_viewport_vds_.clear();
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

        ui.resolver_ctx_.renderer = im.renderer_.get();
        ui.resolver_ctx_.ui = &ui;
        ui.resolver_ctx_.texture_ds_cache_per_frame.resize(im.frames_in_flight_);
        ImGui_ImplVulkan_SetTextureResolverEx(
            ui.imgui_vk_renderer_, &texResolverCallback, &ui.resolver_ctx_);

        for (auto &e : ui.ui_offscreen_views_)
            if (e.ui_pool)
                e.ui_pool->setImGuiRenderer(ui.imgui_vk_renderer_);
    }

    namespace
    {
        using namespace lux::render;
        using Dispatcher = GeneralRenderServer::Dispatcher;
        using Ctx = Dispatcher::Ctx;

        // ── SubmitImGuiDrawData ──────────────────────────────────────────────

        void handleSubmitImGuiDrawData(Ctx &ctx, const SubmitImGuiDrawDataPayload &p)
        {
            auto &im = *static_cast<GeneralRenderServer::Impl *>(ctx.user_state);
            auto *ui = static_cast<UIRenderServer::UIState *>(im.extension_);
            if (!ui)
                return;

            if (p.attachment_index < ctx.program.attachments.size())
            {
                auto &att = ctx.program.attachments[p.attachment_index];
                if (att.type_id == attachment_types::ImGuiDrawData)
                    ui->pending_snapshot = static_cast<ImDrawDataSnapshot *>(att.object);
            }
        }

        // ─────────────────────────────────────────────────────────────────────
        //  Shared UI-offscreen-view helpers — the comm handlers (handleAddUIView /
        //  handleRemoveUIView) and the server-side methods (addUIView / removeUIView)
        //  create and destroy the SAME offscreen views; these hold the common core
        //  so the two sides can no longer drift.
        // ─────────────────────────────────────────────────────────────────────

        // Clamp a requested view extent to the device's framebuffer / image limits.
        static lux::common::Size2D clampViewExtent(
            GeneralRenderServer::Impl &im, lux::common::Size2D extent)
        {
            const auto &limits = im.res_ctx_->deviceContext()
                                     .physicalDevice().properties().properties.limits;
            const uint32_t max_w = std::min(limits.maxFramebufferWidth,  limits.maxImageDimension2D);
            const uint32_t max_h = std::min(limits.maxFramebufferHeight, limits.maxImageDimension2D);
            return {std::clamp(extent.width,  1u, max_w),
                    std::clamp(extent.height, 1u, max_h)};
        }

        // The offscreen render-target layout for a UI view: SAMPLED color (so ImGui
        // can sample it through a descriptor set) + a depth attachment.
        static lux::render::RenderTargetLayout makeUIOffscreenLayout()
        {
            lux::render::RenderTargetLayout layout;

            lux::render::RenderTargetSlotDesc color{};
            color.format         = VK_FORMAT_B8G8R8A8_SRGB;
            color.usage          = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                 | VK_IMAGE_USAGE_SAMPLED_BIT;
            color.aspect         = VK_IMAGE_ASPECT_COLOR_BIT;
            color.final_layout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            color.is_presentable = false;
            layout.slots[static_cast<size_t>(lux::render::TargetSlot::SceneColor)] = color;

            lux::render::RenderTargetSlotDesc depth{};
            depth.format         = VK_FORMAT_D32_SFLOAT;
            depth.usage          = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            depth.aspect         = VK_IMAGE_ASPECT_DEPTH_BIT;
            depth.final_layout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.is_presentable = false;
            layout.slots[static_cast<size_t>(lux::render::TargetSlot::SceneDepth)] = depth;

            return layout;
        }

        // Tear down one UI offscreen view: drop its index, retire its pool for
        // deferred GPU cleanup, swap-pop it out of ui_offscreen_views_, then remove
        // the underlying scene view. No-op if the scene is gone or the view absent.
        static void removeUIViewImpl(
            GeneralRenderServer::Impl  &im,
            UIRenderServer::UIState    &ui,
            lux::render::RenderSceneId  scene_id,
            lux::render::ViewHandle     view_id)
        {
            auto *sc = im.renderer_->getScene(scene_id);
            if (!sc)
                return;

            auto it = std::find_if(
                ui.ui_offscreen_views_.begin(), ui.ui_offscreen_views_.end(),
                [&](const auto &e)
                { return e.scene_id == scene_id && e.view_id == view_id; });
            if (it != ui.ui_offscreen_views_.end())
            {
                ui.scene_view_index_.erase(
                    UIRenderServer::UIState::makeSceneViewKey(it->scene_id, it->view_id.index));

                if (it->ui_pool)
                    ui.retired_ui_pools_.push_back({std::move(it->ui_pool), im.current_stamp_.serial});

                if (it != ui.ui_offscreen_views_.end() - 1)
                {
                    const auto moved_key = UIRenderServer::UIState::makeSceneViewKey(
                        ui.ui_offscreen_views_.back().scene_id,
                        ui.ui_offscreen_views_.back().view_id.index);
                    *it = std::move(ui.ui_offscreen_views_.back());
                    ui.scene_view_index_[moved_key] =
                        static_cast<size_t>(it - ui.ui_offscreen_views_.begin());
                }
                ui.ui_offscreen_views_.pop_back();
            }

            sc->removeView(view_id);
        }

        // ── handleAddUIView — creates UIOffscreenImagePool ─────────────────────

        void handleAddUIView(Ctx &ctx, const AddViewPayload &p)
        {
            auto &im = *static_cast<GeneralRenderServer::Impl *>(ctx.user_state);
            auto *ui = static_cast<UIRenderServer::UIState *>(im.extension_);
            if (!ui)
                return;

            auto *sc = im.renderer_->getScene(p.scene_id);
            if (!sc)
                return;

            const lux::common::Size2D clamped_extent = clampViewExtent(im, p.extent);

            ViewCreateInfo ci{
                .initial_extent = clamped_extent,
                .debug_name = p.name,
            };
            ViewHandle handle = sc->addView(ci);

            RenderTargetLayout layout = makeUIOffscreenLayout();

            VkExtent2D vk_extent{clamped_extent.width, clamped_extent.height};
            auto pool = std::make_unique<UIOffscreenImagePool>(
                *im.res_ctx_, layout, vk_extent, im.frames_in_flight_);
            pool->setImGuiRenderer(ui->imgui_vk_renderer_);
            sc->compileGraphTemplate(layout);

            auto *view = sc->getView(handle);
            if (view)
            {
                const size_t idx = ui->ui_offscreen_views_.size();
                ui->ui_offscreen_views_.push_back(
                    {p.scene_id, handle, std::move(pool), layout});
                ui->scene_view_index_[UIRenderServer::UIState::makeSceneViewKey(p.scene_id, handle.index)] = idx;
            }

            // (Initial camera removed from AddView — View 去 3D 化: the client sends a
            // StandardViewCamera op for this view after addView. AddView is neutral.)

            replyToCurrent<AddViewPayload>(
                ctx, ViewCreatedReply{handle});
        }

        // ── handleRemoveUIView ──────────────────────────────────────────────

        void handleRemoveUIView(Ctx &ctx, const RemoveViewPayload &p)
        {
            auto &im = *static_cast<GeneralRenderServer::Impl *>(ctx.user_state);
            auto *ui = static_cast<UIRenderServer::UIState *>(im.extension_);
            if (!ui)
                return;
            removeUIViewImpl(im, *ui, p.scene_id, p.view);
        }

        // ── Pre-destroy-scene callback ──────────────────────────────────────

        void uiPreDestroyScene(void *extension, lux::render::RenderSceneId scene_id)
        {
            auto *ui = static_cast<UIRenderServer::UIState *>(extension);
            if (!ui)
                return;

            auto it = ui->ui_offscreen_views_.begin();
            while (it != ui->ui_offscreen_views_.end())
            {
                if (it->scene_id != scene_id)
                {
                    ++it;
                    continue;
                }

                if (it->ui_pool)
                    ui->retired_ui_pools_.push_back({std::move(it->ui_pool), 0});

                ui->scene_view_index_.erase(
                    UIRenderServer::UIState::makeSceneViewKey(it->scene_id, it->view_id.index));

                if (it != ui->ui_offscreen_views_.end() - 1)
                {
                    const auto moved_key = UIRenderServer::UIState::makeSceneViewKey(
                        ui->ui_offscreen_views_.back().scene_id,
                        ui->ui_offscreen_views_.back().view_id.index);
                    *it = std::move(ui->ui_offscreen_views_.back());
                    ui->scene_view_index_[moved_key] = static_cast<size_t>(
                        it - ui->ui_offscreen_views_.begin());
                }
                ui->ui_offscreen_views_.pop_back();
            }

            // Also clean up swapchain layer if it references the destroyed scene
            if (ui->swapchain_layer_.has_value() &&
                ui->swapchain_layer_->scene_id == scene_id)
            {
                // Don't remove view — scene is being destroyed and will clean up its own views
                ui->swapchain_layer_.reset();
                ui->overlay_layout_dirty_ = true;
            }
        }

        // ── handleUIRequestSwapchainScene — UI-aware swapchain binding ──────
        void handleUIRequestSwapchainScene(Ctx &ctx, const RequestSwapchainScenePayload &p)
        {
            auto &im = *static_cast<GeneralRenderServer::Impl *>(ctx.user_state);
            auto *ui = static_cast<UIRenderServer::UIState *>(im.extension_);

            SwapchainBoundReply reply{};

            if (!im.swapchain_provider_)
            {
                reply.status = 1;
                replyToCurrent<RequestSwapchainScenePayload>(ctx, reply);
                return;
            }

            auto *scene = im.renderer_->getScene(p.scene_id);
            if (!scene)
            {
                reply.status = 2;
                replyToCurrent<RequestSwapchainScenePayload>(ctx, reply);
                return;
            }

            auto *sc = im.swapchain_provider_.get();
            const auto sc_extent = sc->extent();

            // Create view
            ViewCreateInfo ci{
                .initial_extent = {sc_extent.width, sc_extent.height},
                .debug_name = "SwapchainSceneView",
            };
            ViewHandle view_id = scene->addView(ci);

            // Build layout: COLOR_ATTACHMENT_OPTIMAL if ImGui overlay is enabled
            RenderTargetLayout scene_layout;
            {
                RenderTargetSlotDesc color{};
                color.format = sc->format();
                color.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                bool overlay = ui && ui->imgui_overlay_enabled_;
                color.final_layout = overlay
                    ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                    : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                color.is_presentable = !overlay;
                color.preserve_content = false;
                scene_layout.slots[static_cast<size_t>(TargetSlot::SceneColor)] = color;
            }

            // Unbind any existing swapchain binding
            if (im.swapchain_binding_.has_value())
                im.swapchain_binding_.reset();

            // Compile graph template and set binding
            scene->compileGraphTemplate(scene_layout);
            im.swapchain_binding_ = GeneralRenderServer::Impl::SwapchainBinding{
                .scene_id = p.scene_id,
                .view_id  = view_id,
                .layout   = scene_layout,
            };

            // Remove from offscreen_views_ if present
            auto it = std::remove_if(
                im.offscreen_views_.begin(), im.offscreen_views_.end(),
                [&](const GeneralRenderServer::Impl::OffscreenViewEntry& e) {
                    return e.scene_id == p.scene_id && e.view_id == view_id;
                }
            );
            im.offscreen_views_.erase(it, im.offscreen_views_.end());

            // Store swapchain layer for UIRenderServer
            if (ui)
            {
                ui->swapchain_layer_ = UIRenderServer::UIState::SwapchainLayer{
                    .scene_id = p.scene_id,
                    .view_id = view_id,
                    .layout = scene_layout,
                };

                if (ui->imgui_overlay_enabled_)
                    recompileImGuiOverlayGraph(*ui, sc);
            }

            reply.view   = view_id;
            reply.status = 0;
            replyToCurrent<RequestSwapchainScenePayload>(ctx, reply);
        }
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
        out_ops[1] = d.allocateAndRegisterUnary<
            AddViewPayload, &handleAddUIView>(
            opcodes::CommandOp, imgui_ops::kAddUIView);
        out_ops[2] = d.allocateAndRegisterUnary<
            RemoveViewPayload, &handleRemoveUIView>(
            opcodes::CommandOp, imgui_ops::kRemoveUIView);
        return 3;
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
        std::shared_ptr<Channel> channel,
        std::shared_ptr<lux::render::RenderChannelSync> sync)
        : GeneralRenderServer(std::move(channel), std::move(sync)), ui_(std::make_unique<UIState>())
    {
        impl_->extension_ = ui_.get();
        impl_->pre_destroy_scene_cb_ = &uiPreDestroyScene;
        // (ImGui + UIView handlers now registered dynamically via kImGuiFeatureFactory::register_ops_fn)
    }

    UIRenderServer::~UIRenderServer()
    {
        // Ensure no in-flight command still references ImGui descriptors/images.
        if (impl_ && impl_->dev_ctx_)
            impl_->dev_ctx_->logicalDevice().waitIdle();

        // Cancel deferred viewport-destroy callbacks — they capture the
        // imgui_vk_renderer_ pointer which we are about to destroy.
        // All viewport resources will be cleaned up directly below.
        if (impl_ && impl_->render_ctx_)
            impl_->render_ctx_->retireScheduler().purge(kViewportRetireOwner);

        // Destroy still-open secondary viewports (and any whose deferred
        // destroy we just cancelled).  GPU is idle so this is safe.
        if (ui_->imgui_vk_renderer_)
        {
            for (void* vd : ui_->active_viewport_vds_)
                ImGui_ImplVulkan_DestroyViewportResourcesEx(ui_->imgui_vk_renderer_, vd);
        }
        ui_->active_viewport_vds_.clear();

        // Remove swapchain scene layer's view before scene destruction
        if (ui_->swapchain_layer_.has_value())
        {
            auto &layer = *ui_->swapchain_layer_;
            if (layer.view_id.valid())
            {
                if (auto *scene = impl_->renderer_->getScene(layer.scene_id))
                    scene->removeView(layer.view_id);
            }
            ui_->swapchain_layer_.reset();
        }

        if (ui_->imgui_vk_renderer_)
            clearAllTextureCaches(ui_->imgui_vk_renderer_, ui_->resolver_ctx_);

        // Destroy UIOffscreenImagePools BEFORE the ImGui renderer — they call
        // ImGui_ImplVulkan_RemoveTextureEx which requires a live renderer.
        ui_->ui_offscreen_views_.clear();
        ui_->scene_view_index_.clear();
        ui_->retired_ui_pools_.clear();

        if (ui_->imgui_vk_renderer_)
        {
            ImGui_ImplVulkan_DestroyRendererEx(ui_->imgui_vk_renderer_);
            ui_->imgui_vk_renderer_ = nullptr;
        }
        impl_->extension_ = nullptr;
    }

    ImGuiOperationIds UIRenderServer::imguiOps() const noexcept { return ui_->imgui_ops_; }

    // ─────────────────────────────────────────────────────────────────────────
    //  Swapchain scene — direct rendering to swapchain surface
    // ─────────────────────────────────────────────────────────────────────────

    /// Helper: recompile ImGui overlay graph with appropriate layout.
    /// When a swapchain scene layer exists, overlay must LOAD (preserve scene content);
    /// otherwise, it CLEARs as usual.
    static void recompileImGuiOverlayGraph(UIRenderServer::UIState &ui,
                                           lux::render::SwapchainProvider *sc)
    {
        if (!ui.imgui_scene_ || !sc)
            return;

        auto base_layout = sc->layout(); // SceneColor → PRESENT_SRC_KHR
        auto &color = base_layout.slots[static_cast<size_t>(lux::render::TargetSlot::SceneColor)];
        if (color.has_value() && ui.swapchain_layer_.has_value())
        {
            // Overlay renders on top of scene content — use LOAD
            color->initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color->preserve_content = true;
        }
        // else: no scene layer → default UNDEFINED / CLEAR

        ui.overlay_layout_ = base_layout;
        ui.imgui_scene_->compileGraphTemplate(base_layout);
        ui.overlay_layout_dirty_ = false;
    }

    lux::render::ViewHandle UIRenderServer::addUIView(
        lux::render::RenderSceneId scene_id,
        lux::common::Size2D        extent,
        const char*                debug_name)
    {
        // Server-side equivalent of `handleAddUIView` (above). The
        // handler version reads everything off the `ExecuteContext`'s
        // payload; here we already have the args directly. Logic is
        // kept in sync.
        auto& im  = *impl_;
        auto& ui  = *ui_;

        auto* sc = im.renderer_->getScene(scene_id);
        if (!sc)
            return lux::render::ViewHandle{};

        // Clamp against device limits + the SAMPLED-color offscreen layout — the
        // shared helpers also used by handleAddUIView, so the two cannot drift.
        const lux::common::Size2D clamped = clampViewExtent(im, extent);

        lux::render::ViewCreateInfo ci{
            .initial_extent = clamped,
            .debug_name     = debug_name ? debug_name : "EditorUIView",
        };
        const lux::render::ViewHandle handle = sc->addView(ci);

        lux::render::RenderTargetLayout layout = makeUIOffscreenLayout();

        VkExtent2D vk_extent{clamped.width, clamped.height};
        auto pool = std::make_unique<UIOffscreenImagePool>(
            *im.res_ctx_, layout, vk_extent, im.frames_in_flight_);
        pool->setImGuiRenderer(ui.imgui_vk_renderer_);
        sc->compileGraphTemplate(layout);

        auto* view = sc->getView(handle);
        if (!view)
            return lux::render::ViewHandle{};

        const size_t idx = ui.ui_offscreen_views_.size();
        ui.ui_offscreen_views_.push_back(
            {scene_id, handle, std::move(pool), layout});
        ui.scene_view_index_[UIState::makeSceneViewKey(scene_id, handle.index)] = idx;

        return handle;
    }

    void UIRenderServer::removeUIView(
        lux::render::RenderSceneId scene_id,
        lux::render::ViewHandle    view)
    {
        // Shared core with handleRemoveUIView.
        removeUIViewImpl(*impl_, *ui_, scene_id, view);
    }

    lux::render::ViewHandle UIRenderServer::setSwapchainScene(lux::render::RenderSceneId scene_id)
    {
        const lux::render::ViewHandle kInvalid{};

        if (!impl_->swapchain_provider_)
            return kInvalid;  // no swapchain attached

        if (ui_->swapchain_layer_.has_value())
            return kInvalid;  // already set — call clearSwapchainScene() first

        auto *scene = impl_->renderer_->getScene(scene_id);
        if (!scene)
            return kInvalid;

        auto *sc = impl_->swapchain_provider_.get();
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

        // Build layout: color → COLOR_ATTACHMENT_OPTIMAL (not PRESENT — ImGui still follows)
        lux::render::RenderTargetLayout scene_layout;
        {
            lux::render::RenderTargetSlotDesc color{};
            color.format = sc->format();
            color.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            // When ImGui overlay is enabled, scene output stays as COLOR_ATTACHMENT_OPTIMAL
            // so ImGui can render on top.  When disabled, go straight to PRESENT_SRC_KHR.
            color.final_layout = ui_->imgui_overlay_enabled_
                ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            color.is_presentable = !ui_->imgui_overlay_enabled_;
            color.preserve_content = false;
            scene_layout.slots[static_cast<size_t>(lux::render::TargetSlot::SceneColor)] = color;
            // SceneDepth: intentionally omitted — render graph creates its own
        }

        // Register with base class swapchain binding (unbind any previous first)
        if (hasSwapchainBinding())
            unbindSwapchain();

        auto bind_result = bindSwapchain(scene_id, view_id, scene_layout);
        if (!bind_result)
        {
            scene->removeView(view_id);
            return kInvalid;
        }

        ui_->swapchain_layer_ = UIState::SwapchainLayer{
            .scene_id = scene_id,
            .view_id = view_id,
            .layout = scene_layout,
        };

        // Recompile ImGui overlay graph to use LOAD if overlay is enabled
        if (ui_->imgui_overlay_enabled_)
            recompileImGuiOverlayGraph(*ui_, sc);

        return view_id;
    }

    void UIRenderServer::clearSwapchainScene()
    {
        if (!ui_->swapchain_layer_.has_value())
            return;

        auto &layer = *ui_->swapchain_layer_;
        if (layer.view_id.valid())
        {
            if (auto *scene = impl_->renderer_->getScene(layer.scene_id))
                scene->removeView(layer.view_id);
        }

        ui_->swapchain_layer_.reset();

        // Recompile ImGui overlay graph to remove LOAD (back to CLEAR)
        if (ui_->imgui_overlay_enabled_ && impl_->swapchain_provider_)
            recompileImGuiOverlayGraph(*ui_, impl_->swapchain_provider_.get());
    }

    bool UIRenderServer::hasSwapchainScene() const noexcept
    {
        return ui_->swapchain_layer_.has_value();
    }

    void UIRenderServer::setImGuiOverlayEnabled(bool enabled)
    {
        if (ui_->imgui_overlay_enabled_ == enabled)
            return;

        ui_->imgui_overlay_enabled_ = enabled;

        // Update swapchain scene's final_layout based on overlay state
        if (ui_->swapchain_layer_.has_value())
        {
            auto &layer = *ui_->swapchain_layer_;
            auto &color = layer.layout.slots[static_cast<size_t>(lux::render::TargetSlot::SceneColor)];
            if (color.has_value())
            {
                color->final_layout = enabled
                    ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                    : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                color->is_presentable = !enabled;
            }
            if (auto *scene = impl_->renderer_->getScene(layer.scene_id))
                scene->compileGraphTemplate(layer.layout);
        }

        // Recompile ImGui overlay graph
        if (impl_->swapchain_provider_)
            recompileImGuiOverlayGraph(*ui_, impl_->swapchain_provider_.get());
    }

    bool UIRenderServer::isImGuiOverlayEnabled() const noexcept
    {
        return ui_->imgui_overlay_enabled_;
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

        // 1b. Override RequestSwapchainScene handler with UI-aware version
        impl_->dispatcher.registerUnary<
            lux::render::RequestSwapchainScenePayload, &handleUIRequestSwapchainScene>(
            lux::render::opcodes::CommandOp,
            lux::render::type_ids::RequestSwapchainScene,
            "RequestSwapchainScene");

        // 2. Register the ImGui feature factory
        auto imgui_reg = addFeatureFactory(kImGuiFeatureFactory);
        uint32_t imgui_type_id = imgui_reg.feature_type_id;
        ui_->imgui_ops_ = ImGuiOperationIds::fromOps(imgui_reg.ops, imgui_reg.op_count);

        // 3. Create the ImGui overlay scene with the ImGui feature
        FeatureInitParam fp{imgui_type_id, &imgui_config, sizeof(imgui_config)};
        auto scene_result = createScene("ImGuiOverlay", {&fp, 1});
        ui_->imgui_scene_id_ = scene_result.scene_id;
        ui_->imgui_scene_ = impl_->renderer_->getScene(scene_result.scene_id);
        ui_->imgui_feature_ = ui_->imgui_scene_->getFeatureAs<ImGuiFeature>(scene_result.features[0]);

        // 4. Store color format now; renderer creation is deferred until swapchain is attached.
        if (ui_->imgui_feature_)
        {
            auto &cfg = ui_->imgui_feature_->initConfig();
            ui_->imgui_color_format_ = cfg.color_format;
            (void)cfg;
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

        if (impl_->swapchain_provider_)
        {
            const uint32_t image_count = impl_->swapchain_provider_->imageCount();
            rebuildImGuiRendererForSwapchain(*ui_, *impl_, image_count);
            impl_->swapchain_provider_->setRebuildCallback([this]()
            {
                auto *sc = impl_->swapchain_provider_.get();
                if (!sc)
                    return;
                const uint32_t image_count = sc->imageCount();
                rebuildImGuiRendererForSwapchain(*ui_, *impl_, image_count);
                if (ui_->imgui_overlay_enabled_)
                    recompileImGuiOverlayGraph(*ui_, sc);
            });
        }

        // Create the ImGui swapchain view eagerly now that swapchain_provider_ exists.
        if (ui_->imgui_scene_ && !ui_->imgui_view_id_.valid() && impl_->swapchain_provider_)
        {
            lux::render::ViewCreateInfo ci{
                .initial_extent = {impl_->swapchain_provider_->extent().width, impl_->swapchain_provider_->extent().height},
                .debug_name = "ImGuiSwapchainView",
            };
            ui_->imgui_view_id_ = ui_->imgui_scene_->addView(ci);
            ui_->imgui_view_ = ui_->imgui_scene_->getView(ui_->imgui_view_id_);
            if (!ui_->imgui_view_)
            {
                ui_->imgui_view_id_ = {};
                return lux::cxx::unexpected(make_error_code(lux::render::ERenderError::InternalError));
            }

            // Initialize overlay layout from swapchain defaults (CLEAR, PRESENT_SRC_KHR)
            ui_->overlay_layout_ = impl_->swapchain_provider_->layout();
        }

        return {};
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  tick — override with ImGui support
    // ─────────────────────────────────────────────────────────────────────────

    bool UIRenderServer::tick()
    {
        assert(ui_ && "UIState should be initialized in the constructor");
        assert(ui_->imgui_feature_ && "ImGuiFeature should be created in init()");
        assert(ui_->imgui_vk_renderer_ && "ImGui VkRenderer should be created in init()");

        // Generate authoritative stamp before draining requests so handlers
        // (AddView/ViewFrameUpdate) always see the current tick slot.
        auto &im = *impl_;
        if (!lux::render::RenderTickPipeline::runTick(
                im.frame_orchestrator_, im.current_stamp_,
                [&]()
                { return drainRequestBlocking(); }, 0))
        {
            return false;
        }

        const auto &limits = im.res_ctx_->deviceContext()
            .physicalDevice()
            .properties()
            .properties.limits;
        const uint32_t max_w = std::min(limits.maxFramebufferWidth, limits.maxImageDimension2D);
        const uint32_t max_h = std::min(limits.maxFramebufferHeight, limits.maxImageDimension2D);
        auto sanitize_extent = [&](VkExtent2D ext)
        {
            ext.width = std::clamp(ext.width, 1u, max_w);
            ext.height = std::clamp(ext.height, 1u, max_h);
            return ext;
        };

        flushDeferredRepliesOnly();

        bool has_offscreen = !im.offscreen_views_.empty() || !ui_->ui_offscreen_views_.empty();
        bool has_swapchain = (ui_->imgui_view_ || ui_->swapchain_layer_.has_value()) && im.swapchain_provider_;
        if (!has_offscreen && !has_swapchain)
        {
            if (ui_->imgui_vk_renderer_)
                clearAllTextureCaches(ui_->imgui_vk_renderer_, ui_->resolver_ctx_);
            return true;
        }

        // Handle all pending resizes before beginning the frame.
        // Single-pass loops reduce duplicate scene/view lookups.
        if (im.frame_driver_)
        {
            for (auto &e : im.offscreen_views_)
            {
                auto *scene = im.renderer_->getScene(e.scene_id);
                auto *view = scene ? scene->getView(e.view_id) : nullptr;
                if (!view || !view->resize_pending)
                    continue;

                VkExtent2D ext = sanitize_extent({view->current_extent.width, view->current_extent.height});
                e.pool->resize(ext);
                view->resize_pending = false;
            }
            for (auto &e : ui_->ui_offscreen_views_)
            {
                auto *scene = im.renderer_->getScene(e.scene_id);
                auto *view = scene ? scene->getView(e.view_id) : nullptr;
                if (!view || !view->resize_pending)
                    continue;

                VkExtent2D ext = sanitize_extent({view->current_extent.width, view->current_extent.height});
                e.ui_pool->resize(ext);
                view->resize_pending = false;
            }
        }

        // Begin frame — single CB for all rendering
        lux::render::SwapchainProvider *sc_ptr = im.swapchain_provider_.get();
        const auto &stamp = im.current_stamp_;
        lux::render::FrameRuntime rt{};
        rt.stamp = stamp;
        if (im.frame_driver_)
        {
            rt = im.frame_driver_->beginFrame(stamp.slotIndex(), stamp.serial, sc_ptr);
            im.frame_orchestrator_.patchImageIndex(rt.image_index);
            im.current_stamp_ = im.frame_orchestrator_.stamp();
            rt.stamp = im.current_stamp_; // re-attach after FrameDriver fills the rest

            // Fence wait is completed in FrameDriver::beginFrame().
            // Only now is it safe to retire this slot's staging/upload state
            // and advance scene frame state.
            im.beginTickFrame();

            if (rt.primary_cmd == VK_NULL_HANDLE)
            {
                im.endTickFrame(false);
                return true;
            }

            if (sc_ptr)
            {
                static thread_local lux::render::RenderTargetBinding sc_binding;
                sc_binding = sc_ptr->makeFrameBinding(rt.image_index);
                rt.present_target = &sc_binding;
            }
        }
        else
        {
            // Offscreen-only path without FrameDriver still advances renderer
            // frame lifecycle, but has no swapchain/image acquire step.
            im.beginTickFrame();
        }

        // Upload phase: global + per-scene transfers, once per tick.
        if (rt.primary_cmd != VK_NULL_HANDLE)
            im.renderer_->runUploadPhase(rt.primary_cmd);

        // Phase 1: Render base offscreen views
        // Build a compact scene/view batch to avoid per-frame hash maps.
        lux::render::SceneViewBatch scene_view_batch;
        for (auto &e : im.offscreen_views_)
        {
            auto *scene = im.renderer_->getScene(e.scene_id);
            auto *view = scene ? scene->getView(e.view_id) : nullptr;
            if (!scene || !view)
                continue;

            scene_view_batch.add(scene, view);
            auto binding = e.pool->makeFrameBinding(im.current_stamp_.slotIndex());
            // Always use cross_view_index=0: each offscreen view owns its own
            // image pool, so imported images start at UNDEFINED (not at the
            // previous view's final state).  first_view_barriers handle this.
            im.renderer_->renderSingleView(
                *scene, *view, binding, rt, 0);
        }

        // Phase 2: Render UI offscreen views + ensure ImGui descriptors
        for (auto &e : ui_->ui_offscreen_views_)
        {
            auto *scene = im.renderer_->getScene(e.scene_id);
            auto *view = scene ? scene->getView(e.view_id) : nullptr;
            if (!scene || !view)
                continue;

            scene_view_batch.add(scene, view);
            auto binding = e.ui_pool->makeFrameBinding(im.current_stamp_.slotIndex());
            // cross_view_index=0: same rationale as Phase 1 — independent pool.
            im.renderer_->renderSingleView(
                *scene, *view, binding, rt, 0);
            e.ui_pool->ensureImGuiDescriptors();
        }

        // No explicit fence wait needed — all rendering is in a single CB.
        // Offscreen writes are guaranteed visible to subsequent ImGui sampling
        // because the render graph's final barriers provide the synchronization.

        // Phase 3: Forward ImGui draw data
        if (ui_->imgui_overlay_enabled_ && ui_->pending_snapshot &&
            ui_->pending_snapshot->drawData().Valid)
        {
            ui_->imgui_feature_->setVkRenderer(ui_->imgui_vk_renderer_);
            ui_->imgui_feature_->setDrawData(&ui_->pending_snapshot->drawData());
        }

        // Phase 4: Render swapchain scene layer (3D content → swapchain)
        if (ui_->swapchain_layer_.has_value() && rt.present_target && sc_ptr)
        {
            auto &layer = *ui_->swapchain_layer_;
            auto *scene = im.renderer_->getScene(layer.scene_id);
            auto *view = scene ? scene->getView(layer.view_id) : nullptr;
            if (scene && view)
            {
                scene_view_batch.add(scene, view);
                // cross_view_index=0: swapchain layer uses its own target binding.
                // Swapchain layout varies per overlay phase — set it here (阶段4 P4d).
                rt.present_target->layout = &layer.layout;
                im.renderer_->renderSingleView(
                    *scene, *view, *rt.present_target, rt, 0);
            }
        }

        // Phase 5: Render ImGui overlay on top of swapchain (if enabled)
        ui_->resolver_ctx_.frame_index = im.current_stamp_.slotIndex();
        clearTextureCacheSlot(ui_->imgui_vk_renderer_, getFrameTextureCache(ui_->resolver_ctx_));

        if (ui_->imgui_overlay_enabled_ && has_swapchain && rt.present_target && ui_->imgui_view_)
        {
            // Lazy recompile if overlay layout changed (e.g. scene layer added/removed)
            if (ui_->overlay_layout_dirty_)
                recompileImGuiOverlayGraph(*ui_, sc_ptr);

            scene_view_batch.add(ui_->imgui_scene_, ui_->imgui_view_);
            const auto &item = scene_view_batch.items().back();
            // Overlay composites on top of the swapchain layer — its own layout (阶段4 P4d).
            rt.present_target->layout = &ui_->overlay_layout_;
            im.renderer_->renderSingleView(
                *item.scene, *item.view, *rt.present_target, rt, item.cross_view_index);
        }

        // End-of-view-frame for each touched scene
        for (auto *scene : scene_view_batch.touchedScenes())
            scene->endViewFrame(im.current_stamp_.slotIndex());

        // End frame — end CB, submit (with semaphore chain if swapchain), present
        if (im.frame_driver_)
            (void)im.frame_driver_->endFrame(rt, sc_ptr);

        // Render secondary viewports (multi-viewport / platform windows).
        // Fully data-decoupled: the render thread NEVER reads platform_io.Viewports.
        // All data comes from the snapshot (viewport events + viewport frames).
        if (ui_->pending_snapshot && ui_->imgui_vk_renderer_ &&
            (!ui_->pending_snapshot->viewportEvents().empty() ||
             !ui_->pending_snapshot->viewportFrames().empty()))
        {
            // Process viewport lifecycle events bundled in the snapshot.
            // Destroyed events are deferred until the GPU finishes the current frame.
            {
                const auto& events = ui_->pending_snapshot->viewportEvents();
                for (const auto& evt : events)
                {
                    if (evt.type == ImGui_ImplVulkan_ViewportEvent::Created)
                    {
                        ui_->active_viewport_vds_.insert(evt.viewport_data);
                    }
                    else if (evt.type == ImGui_ImplVulkan_ViewportEvent::Destroyed)
                    {
                        void* vd_ptr = evt.viewport_data;
                        ImGui_ImplVulkan_Renderer* renderer = ui_->imgui_vk_renderer_;
                        auto* active_set = &ui_->active_viewport_vds_;
                        im.render_ctx_->retireScheduler().defer(
                            im.current_stamp_.serial,
                            kViewportRetireOwner,
                            [renderer, vd_ptr, active_set]()
                            {
                                ImGui_ImplVulkan_DestroyViewportResourcesEx(renderer, vd_ptr);
                                active_set->erase(vd_ptr);
                            });
                    }
                }

                // Apply pending viewport creates/resizes from events (no platform_io access)
                ImGui_ImplVulkan_SyncViewportResourcesFromEventsEx(
                    ui_->imgui_vk_renderer_,
                    events.Data, events.Size);

                // Render from snapshot — iterate viewportFrames instead of platform_io.Viewports
                const auto& frames = ui_->pending_snapshot->viewportFrames();
                for (int i = 0; i < frames.Size; i++)
                {
                    const auto& f = frames[i];
                    ImGui_ImplVulkan_RenderViewportEx(
                        ui_->imgui_vk_renderer_, f.vd,
                        const_cast<ImDrawData*>(&f.draw_data), f.size, f.flags);
                }
                for (int i = 0; i < frames.Size; i++)
                {
                    ImGui_ImplVulkan_SwapViewportEx(frames[i].vd);
                }
            }
        }

        // GC retired offscreen images now that this frame's submit is done
        for (auto &e : im.offscreen_views_)
            e.pool->collectRetired(im.current_stamp_.serial, im.frames_in_flight_);
        for (auto &e : ui_->ui_offscreen_views_)
            e.ui_pool->collectRetired(im.current_stamp_.serial, im.frames_in_flight_);

        auto retired_it = ui_->retired_ui_pools_.begin();
        while (retired_it != ui_->retired_ui_pools_.end())
        {
            if (!retired_it->ui_pool)
            {
                retired_it = ui_->retired_ui_pools_.erase(retired_it);
                continue;
            }

            retired_it->ui_pool->collectRetired(im.current_stamp_.serial, im.frames_in_flight_);
            if (retired_it->retire_frame == 0)
                retired_it->retire_frame = im.current_stamp_.serial;

            const bool aged_out = (im.current_stamp_.serial >= retired_it->retire_frame + im.frames_in_flight_);
            if (aged_out)
                retired_it = ui_->retired_ui_pools_.erase(retired_it);
            else
                ++retired_it;
        }

        // Async readbacks (ReadbackViewAsync): settle / submit / poll + send the
        // deferred replies. After endFrame so a just-settled copy is ordered
        // behind this tick's render. UIRenderServer reimplements tick() rather
        // than calling GeneralRenderServer::tick(), so this must be invoked here
        // too — otherwise offscreen readbacks (e.g. editor thumbnails) never
        // complete under the UI server.
        pollPendingReadbacks();

        im.endTickFrame(rt.primary_cmd != VK_NULL_HANDLE);

        ui_->pending_snapshot = nullptr;
        return true;
    }

} // namespace lux::ui
