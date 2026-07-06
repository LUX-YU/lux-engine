#include <lux/engine/render/comm/server/RenderServerImpl.hpp>
// (InitialViewCamera.hpp retired — initial camera is a StandardViewCamera op now.)
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/comm/RenderTickPipeline.hpp>

// VMA — readback staging buffer uses raw vmaCreateBuffer/vmaInvalidateAllocation
// directly (previously pulled in transitively via SkinningResources.hpp, which
// moved to a feature; include what we use).
#include <vk_mem_alloc.h>

// Vulkan infrastructure
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/renderer/Renderer.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/ShaderPermutationCompiler.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>

// Targets
#include <lux/engine/render/targets/OffscreenImagePool.hpp>
#include <lux/engine/render/core/RenderSurface.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>
#include <lux/engine/render/renderer/FrameDriver.hpp>

// Window
#include <lux/engine/window/LuxWindow.hpp>

// Resources
#include <lux/engine/render/resources/TextureResources.hpp>
// (LightResources include removed — light is feature-owned now; LightFeature
//  emplaces the per-scene LightResources, not the core server.)
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/pipeline/ShadingModelRegistry.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/pipeline/VertexLayoutRegistry.hpp>   // vertex-layout SSOT
#include <lux/engine/render/pipeline/VertexLayoutSpec.hpp>       // make*VertexLayout()
#include <lux/engine/render/resources/lighting/ShadowResources.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/vertex/MeshInputPool.hpp>       // static input pool
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/descriptor/BindlessCombinedSet.hpp>
#include <lux/engine/render/resources/lifecycle/UploadWorkerPool.hpp>
#include <lux/engine/render/resources/lifecycle/VRAMBudgetGuard.hpp>

// Resource descriptions (rdesc)
#include <lux/engine/description/MaterialEnums.hpp>   // rdesc::EAlphaMode (graph render-state)
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Texture.hpp>

// Shader compilation
#include <lux/engine/render/core/ShaderObject.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/description/Shader.hpp>

// Scene / View
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/renderer/RenderViewTypes.hpp>
#include <lux/engine/render/core/MaterialFamily.hpp>
// (LightDescriptor include removed — light commands are feature-scoped now.)

#include <lux/cxx/container/SparseSet.hpp>

#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>      // DumpRenderGraph → in-memory text capture
#include <limits>
#include <unordered_map>

namespace lux::render
{
    static std::vector<const char*> toCharPtrVec(const std::vector<std::string>& strs)
    {
        std::vector<const char*> result;
        result.reserve(strs.size());
        for (auto& s : strs) result.push_back(s.c_str());
        return result;
    }

    Expected<void> GeneralRenderServer::Impl::init(ServerConfig cfg)
    {
        // 0. Create Vulkan infrastructure (InstanceContext ctor may throw)
        DebugCallback debug_cb = nullptr;
        if (cfg.enable_validation)
        {
            debug_cb = [](const DebugCallbackInfo& info) -> bool {
                const char* severity = "INFO";
                if (info.flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)        severity = "ERROR";
                else if (info.flags & VK_DEBUG_REPORT_WARNING_BIT_EXT) severity = "WARN";
                std::cerr << "[Vulkan " << severity << "] " << info.message << "\n";
                return false;
            };
        }
        inst_ctx_ = std::make_unique<InstanceContext>(cfg.instance_extensions, std::move(debug_cb));
        dev_ctx_  = std::make_unique<DeviceContext>(*inst_ctx_);
        res_ctx_  = std::make_unique<ResourceContext>(*dev_ctx_);

        if (cfg.frames_in_flight < 1 || cfg.frames_in_flight > kMaxFramesInFlight)
            return lux::cxx::unexpected(make_error_code(ERenderError::InvalidArgument));
        frames_in_flight_ = cfg.frames_in_flight;
        frame_orchestrator_ = FrameOrchestrator{cfg.frames_in_flight};
        enable_vsync_     = cfg.enable_vsync;

        if (auto r = dev_ctx_->init(cfg.prefer_discrete_gpu
                              ? EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED
                              : EPhysicalDeviceSelectionPolicy::INTEGRATED_GPU_PREFERRED); !r)
            return lux::cxx::unexpected(r.error());
        // Per-scene descriptor sets (light/scene/instance/vertex-pool/shadow/
        // skinning) now come from each RenderScene's own growable
        // SceneDescriptorArena, so the shared pool only serves the few GLOBAL
        // sets (MaterialResources × FIF; Texture/Bindless own their pools).
        // Default sizing suffices — no per-scene-count bump needed.
        if (auto r = res_ctx_->init(); !r)
            return lux::cxx::unexpected(r.error());

        auto& device_ctx        = *dev_ctx_;
        VkDescriptorPool dp     = res_ctx_->descriptorPool().handle();
        VkCommandPool    cp     = res_ctx_->commandPool().handle();
        uint32_t fif            = cfg.frames_in_flight;

        // 1. Descriptor layouts
        auto descriptor_layouts = std::make_unique<GeneralDescriptorSetLayout>(device_ctx);
        if (!descriptor_layouts->init())
            return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));
        auto& layouts = *descriptor_layouts;

        // 2. Shader compiler + Pipeline manager
        auto permutation_compiler = std::make_unique<ShaderPermutationCompiler>();
        auto pipeline_mgr = std::make_unique<PipelineManager>(device_ctx, cfg.use_dynamic_rendering);
        pipeline_mgr->setPermutationCompiler(permutation_compiler.get());

        // 3. Global resource registry
        auto global_reg = std::make_unique<GlobalResourceRegistry>();

        // TextureResources
        {
            // Use device-queried limits for bindless descriptor capacity.
            const auto& idx_props = device_ctx.physicalDevice().descriptorIndexingProperties();
            const uint32_t raw_budget = std::min(
                idx_props.maxDescriptorSetUpdateAfterBindSampledImages,
                idx_props.maxPerStageDescriptorUpdateAfterBindSampledImages);
            // Reserve headroom for COMBINED_IMAGE_SAMPLER bindings in other
            // sets of the same pipeline layout (e.g. SHADOW_ATLAS in Light set).
            constexpr uint32_t kNonTextureReserve = 8;
            constexpr uint32_t kCubeReserve = 256;
            const uint32_t texture_budget = (raw_budget > kNonTextureReserve)
                                              ? (raw_budget - kNonTextureReserve) : raw_budget;
            const uint32_t tex2d_device = (texture_budget > kCubeReserve)
                                         ? (texture_budget - kCubeReserve) : 1u;
            // Cap the addressable bindless range well above any realistic working
            // set (a few thousand textures). BCS sizes its host slot arrays
            // (slots_/gen_/alive_) to this ceiling at init — the raw device limit
            // (~1M on desktop) reserved ~56 MB up front for nothing. 64K bindless
            // 2D textures is plenty and shrinks that to a few MB. (P-1)
            constexpr uint32_t kBindlessTex2DCeiling = 64u * 1024u;
            const uint32_t tex2d_max = std::min(tex2d_device, kBindlessTex2DCeiling);

            BCInitInfo bc{};
            bc.resource_context      = res_ctx_.get();
            bc.descriptor_set_layout = layouts.getTextureSetLayout();
            bc.set_index             = get_binding_set<ETextureSetBindings>::value;
            bc.binding               = 0;
            bc.layout_max_capacity   = tex2d_max;  // device-aware ceiling
            bc.initial_capacity      = 1024;
            bc.srgb_for_color        = true;
            bc.frames_in_flight      = fif;

            VkSamplerCreateInfo sampler_ci{};
            sampler_ci.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sampler_ci.magFilter     = VK_FILTER_LINEAR;
            sampler_ci.minFilter     = VK_FILTER_LINEAR;
            sampler_ci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_ci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_ci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler_ci.anisotropyEnable        = VK_FALSE;
            sampler_ci.borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
            sampler_ci.unnormalizedCoordinates = VK_FALSE;
            sampler_ci.compareEnable           = VK_FALSE;
            sampler_ci.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;

            TextureResources::InitInfo info{};
            info.device_context     = &device_ctx;
            info.graphics_queue     = device_ctx.graphicsQueue();
            info.upload_cmd_pool    = cp;
            info.combined_ci        = bc;
            info.slices             = fif;
            info.default_sampler_ci = sampler_ci;
            info.fallback_pixel     = std::nullopt;
            global_reg->emplace<TextureResources>();
            global_reg->find<TextureResources>()->init(info);
        }

        // LightResources is PER-SCENE and FEATURE-OWNED: LightFeature emplaces +
        // init's it in initAndAttachTo (with the SHARED light set layout). The core
        // server/scene no longer touches light — a scene without LightFeature renders
        // unlit. No global instance exists.

        // ShadingModelRegistry + MaterialResources (5 family SSBOs + variant buckets)
        // are built LAZILY now — see ensureGlobalMaterialResources(), triggered by
        // the first StandardMaterial attach or the first uploadGraphMaterial. A server
        // whose scenes never add StandardMaterial and never upload a material (pure
        // 2D / headless / compute-only) pays nothing here. (Stage D — material stack
        // is a feature domain; the core no longer names shading models.)

        // MeshResources (vertex 64MB + index 32MB arena) is built LAZILY now —
        // see ensureGlobalMeshResources(), triggered by the first StandardMeshStack
        // attach or the first uploadMesh. A server whose scenes never add
        // StandardMeshStack and never upload a mesh (pure 2D / headless /
        // compute-only) pays nothing here.

        // VertexLayoutRegistry: single source of truth for vertex layouts,
        // consumed by the bindless vertex-pool shaders via spec constants. Filled
        // here — before any scene/feature init — so features can fetch by id.
        // First registration → id 0 (= rdesc::Vertex, full); second → id 1
        // (= lean skinned output, geometry-only).
        {
            global_reg->emplace<VertexLayoutRegistry>();
            auto* vlr = global_reg->find<VertexLayoutRegistry>();
            vlr->registerLayout(makeDefaultVertexLayout());      // id 0
            vlr->registerLayout(makeLeanSkinnedOutputLayout());  // id 1
        }

        // ShadowResources is now PER-SCENE (M1, Plan A): each scene's
        // ShadowMapFeature lazily emplaces a ShadowResources into the scene
        // registry. No global atlas exists anymore.

        // ShaderResources (initialized after RenderContext is created below)
        global_reg->emplace<ShaderResources>();

        // 5. Build RenderContext
        RenderContext::CreateInfo ci{};
        ci.pipeline_mgr         = std::move(pipeline_mgr);
        ci.descriptor_layouts   = std::move(descriptor_layouts);
        ci.global_resources     = std::move(global_reg);
        ci.permutation_compiler = std::move(permutation_compiler);
        ci.frames_in_flight     = fif;

        render_ctx_ = std::make_shared<RenderContext>(*res_ctx_, std::move(ci));

        // 6. Build Renderer
        renderer_ = std::make_unique<Renderer>(render_ctx_);

        // 7. Initialize ShaderResources (needs VkDevice)
        render_ctx_->globalRegistry().find<ShaderResources>()->init(
            ShaderResources::InitInfo{render_ctx_->device()});

        // 7b. Inject centralized DeferredDestroyQueue into global GPU resources.
        //     Resources were created before RenderContext, so we do a late bind.
        {
            auto* q = &render_ctx_->deferredDestroyQueue();
            auto& reg = render_ctx_->globalRegistry();
            // LightResources is per-scene + feature-owned now; its deferred queue is bound in LightFeature.
            // MeshResources + MaterialResources are built lazily (ensureGlobalMesh/
            // MaterialResources) which bind their own deferred queue — nothing to
            // late-bind here.
            if (auto* r = reg.find<TextureResources>())   r->setDeferredQueue(q);
        }

        // 8. Async upload worker pool
        {
            UploadWorkerPool::Config ucfg{};
            ucfg.device_ctx = dev_ctx_.get();
            upload_pool_ = std::make_unique<UploadWorkerPool>(ucfg);
        }

        return {};
    }

    GeneralRenderServer::Impl::~Impl()
    {
        if (!dev_ctx_) return;  // init() was never called

        // Shut down the upload worker pool FIRST — this joins worker threads
        // (pool_.close()) then calls vkDeviceWaitIdle internally, ensuring no
        // concurrent vkQueueSubmit2 from workers during teardown.
        if (upload_pool_)
        {
            upload_pool_->shutdown();

            auto vma = dev_ctx_->vmaAllocator();

            // After shutdown all GPU work is done — free staging from pending completions.
            for (auto& c : pending_completions_)
                if (c.stg_buf != VK_NULL_HANDLE)
                    vmaDestroyBuffer(vma, c.stg_buf, c.stg_alloc);
            pending_completions_.clear();

            // Drain any leftover completions from the MPSC ring.
            uint32_t n;
            do {
                n = upload_pool_->drainCompletions(completion_buf_, kMaxDrainBatch);
                for (uint32_t i = 0; i < n; ++i)
                    if (completion_buf_[i].stg_buf != VK_NULL_HANDLE)
                        vmaDestroyBuffer(vma, completion_buf_[i].stg_buf,
                                         completion_buf_[i].stg_alloc);
            } while (n > 0);

            upload_pool_.reset();
        }

        dev_ctx_->logicalDevice().waitIdle();

        // Free any in-flight async readbacks (GPU is idle after waitIdle above;
        // their deferred replies are simply dropped — the client gives up on
        // shutdown). res_ctx_ is still alive here (destroyed in reverse order).
        {
            VmaAllocator  vma  = dev_ctx_->vmaAllocator();
            VkDevice      dev  = dev_ctx_->logicalDevice();
            VkCommandPool pool = res_ctx_->commandPool();
            for (auto& j : pending_readbacks_)
            {
                if (j.fence) vkDestroyFence(dev, j.fence, nullptr);
                if (j.cb)    vkFreeCommandBuffers(dev, pool, 1, &j.cb);
                if (j.buf)   vmaDestroyBuffer(vma, j.buf, j.alloc);
            }
            pending_readbacks_.clear();
        }

        // Free deferred staging buffers + deferred offscreen pools in all FIF slots.
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        {
            async_deferred_staging_[i].clear();
            async_deferred_pools_[i].clear();   // P0-3 deferred DestroyScene pools
        }

        // Destroy offscreen pools before device teardown
        offscreen_views_.clear();

        // Destroy FrameDriver (waits idle + frees sync objects)
        frame_driver_.reset();

        swapchain_provider_.reset();
        if (surface_.isValid() && attached_window_)
            surface_.destroy(*attached_window_, inst_ctx_->instance());
        // renderer_ and render_ctx_ are destroyed implicitly in reverse
        // declaration order: renderer_ first (→ scenes → features cleaned up
        // while render_ctx_ is still alive), then render_ctx_.
    }

    // ─────────────────────────────────────────────────────────────────────
    //  lookupScene — exported for feature operation handlers
    // ─────────────────────────────────────────────────────────────────────

    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id)
    {
        auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
        return im.renderer_->getScene(scene_id);
    }

    // The scene set by SetActiveScene — the transform batch (BulkData) carries no
    // per-entry scene_id, so its feature handler resolves the scene through here.
    RenderScene* lookupCurrentBulkScene(void* user_state)
    {
        auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
        return im.renderer_->getScene(im.current_bulk_scene_);
    }

    // (serverAddMeshInstance + ensureGlobalMeshResources moved to the StandardMeshStack
    //  feature — renderer/features/meshstack/MeshStackOperationHandlers.cpp (Stage C).
    //  The whole mesh assembly is feature-internal now; it reaches the server's Vulkan
    //  stack via GeneralRenderServer::Impl, which is project-visible in RenderServerImpl.hpp.)

    // (ensureGlobalMaterialResources + serverUploadGraphMaterial / serverModifyGraphMaterial
    //  / serverDestroyMaterial moved to the StandardMaterial feature —
    //  renderer/features/material/MaterialOperationHandlers.cpp (Stage C). The whole
    //  material assembly is feature-internal now; it reaches the server's Vulkan stack via
    //  GeneralRenderServer::Impl (project-visible in RenderServerImpl.hpp).)

    // (serverUploadMesh / serverDestroyMesh + the concatMeshLods helper moved to the
    //  StandardMeshStack feature — MeshStackOperationHandlers.cpp (Stage C).)

    // ─────────────────────────────────────────────────────────────────────
    //  Server-level handlers
    //
    //  Convention: ctx.user_state = GeneralRenderServer::Impl*
    // ─────────────────────────────────────────────────────────────────────

    namespace
    {
        using Dispatcher = GeneralRenderServer::Dispatcher;
        using Ctx = Dispatcher::Ctx;

        inline GeneralRenderServer::Impl& impl(Ctx& ctx)
        {
            return *static_cast<GeneralRenderServer::Impl*>(ctx.user_state);
        }

        // (resolveInstanceSlot moved with the mesh-instance handlers to the
        //  StandardMeshStack feature — MeshStackOperationHandlers.cpp.)

        // (invalidateAllSceneGraphs moved to the StandardMaterial feature with the material
        //  assembly — MaterialOperationHandlers.cpp inlines forEachScene+invalidateGraph
        //  directly. The core had no other caller.)

        // (instanceUsesMaterialSlot + repointMaterialSlotInAllScenes deleted — they
        //  were the only remaining core callers of packMaterialType / EShadingModel
        //  and had ZERO call sites themselves. The core no longer names shading
        //  models; the material_type instance field is an opaque uint32 the material
        //  resource layer pre-packs. See Stage D.)

        // EPixelFormat → VkFormat conversion for texture upload handlers
        VkFormat pixelFormatToVk(EPixelFormat fmt)
        {
            switch (fmt) {
            case EPixelFormat::RGBA8_SRGB:     return VK_FORMAT_R8G8B8A8_SRGB;
            case EPixelFormat::RGBA8_UNORM:    return VK_FORMAT_R8G8B8A8_UNORM;
            case EPixelFormat::RGBA16_SFLOAT:  return VK_FORMAT_R16G16B16A16_SFLOAT;
            case EPixelFormat::RG8_UNORM:      return VK_FORMAT_R8G8_UNORM;
            case EPixelFormat::R8_UNORM:       return VK_FORMAT_R8_UNORM;
            case EPixelFormat::BC1_SRGB:       return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case EPixelFormat::BC3_SRGB:       return VK_FORMAT_BC3_SRGB_BLOCK;
            case EPixelFormat::BC5_UNORM:      return VK_FORMAT_BC5_UNORM_BLOCK;
            case EPixelFormat::BC7_SRGB:       return VK_FORMAT_BC7_SRGB_BLOCK;
            default:                           return VK_FORMAT_R8G8B8A8_SRGB;
            }
        }

        // ── Scene lifecycle ──────────────────────────────────────────────────

        // Helper: set up offscreen render targets for a view.
        // Shared by handleCreateScene (initial views) and handleAddView.
        void setupOffscreenViewTarget(GeneralRenderServer::Impl& im,
                             RenderScene* sc, RenderSceneId scene_id,
                             ViewHandle handle, common::Size2D extent)
        {
            RenderTargetLayout layout;
            {
                RenderTargetSlotDesc color{};
                color.format       = VK_FORMAT_B8G8R8A8_SRGB;
                color.usage        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                color.aspect       = VK_IMAGE_ASPECT_COLOR_BIT;
                color.final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.is_presentable = false;
                layout.slots[static_cast<size_t>(TargetSlot::SceneColor)] = color;

                RenderTargetSlotDesc depth{};
                depth.format       = VK_FORMAT_D32_SFLOAT;
                depth.usage        = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                depth.aspect       = VK_IMAGE_ASPECT_DEPTH_BIT;
                depth.final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depth.is_presentable = false;
                layout.slots[static_cast<size_t>(TargetSlot::SceneDepth)] = depth;
            }

            VkExtent2D vk_extent{extent.width, extent.height};

            auto pool = std::make_unique<OffscreenImagePool>(
                *im.res_ctx_, layout, vk_extent, im.frames_in_flight_);

            sc->compileGraphTemplate(layout);

            if (sc->getView(handle))
                im.offscreen_views_.push_back({scene_id, handle, std::move(pool), layout});
        }

        void handleCreateScene(Ctx& ctx, const CreateScenePayload& p)
        {
            auto& im = impl(ctx);
            RenderScene::Config config{};
            config.scene_name = p.name;
            config.pipeline.lit_color_format = p.lit_color_format;

            // Convert fixed-size ViewInit array → vector<ViewCreateInfo>
            const auto n = std::min(p.view_count, uint32_t{4});
            for (uint32_t i = 0; i < n; ++i)
            {
                auto& vi = p.views[i];
                config.initial_views.push_back(ViewCreateInfo{
                    .initial_extent = vi.extent,
                    .debug_name     = vi.name,
                });
            }

            auto result = im.renderer_->addScene(std::move(config));
            auto* sc    = im.renderer_->getScene(result.scene_id);

            SceneCreatedReply reply{};
            reply.scene_id   = result.scene_id;
            reply.view_count = static_cast<uint32_t>(result.view_handles.size());
            for (uint32_t i = 0; i < reply.view_count && i < 4; ++i)
            {
                reply.views[i] = ViewHandle{result.view_handles[i]};

                // All views created via protocol are offscreen
                if (sc)
                    setupOffscreenViewTarget(im, sc, result.scene_id,
                                    result.view_handles[i],
                                    p.views[i].extent);
            }

            replyToCurrent<CreateScenePayload>(ctx, reply);
        }

        void handleDestroyScene(Ctx& ctx, const DestroyScenePayload& p)
        {
            auto& im = impl(ctx);
            if (!im.renderer_->getScene(p.scene_id)) return;

            // NO full-device vkDeviceWaitIdle here (P0-3): it stalled every OTHER scene
            // on each teardown. Everything the GPU might still reference is retired
            // ASYNChronously instead, freed only once the GPU has passed the frames
            // that could touch it:
            //   - the scene's own GPU resources  → removeScene() retires the scene and
            //     defers shutdownFull() by fif frames (PR-6);
            //   - the server-side offscreen pools → moved into the per-FIF deferred
            //     ring below (freed when the slot's fence is next waited);
            //   - subclass per-scene pools (UI)   → pre_destroy_scene_cb_ retires them
            //     into its own fif-gated list.
            // The swapchain binding owns no GPU pool (the swapchain images belong to
            // the provider), so dropping it is immediate and safe.
            im.renderer_->removeScene(p.scene_id);

            // Notify subclass before removing cached pointers
            if (im.pre_destroy_scene_cb_ && im.extension_)
                im.pre_destroy_scene_cb_(im.extension_, p.scene_id);

            // Defer this scene's offscreen pools into the current FIF slot's ring
            // (freed fif frames later), then drop the now-empty entries.
            const uint32_t slot = im.current_stamp_.slotIndex();
            for (auto& e : im.offscreen_views_)
                if (e.scene_id == p.scene_id && e.pool)
                    im.async_deferred_pools_[slot].push_back(std::move(e.pool));
            std::erase_if(im.offscreen_views_, [&](const auto& e) {
                return e.scene_id == p.scene_id;
            });

            // Clear swapchain binding if it belonged to this scene (no GPU pool owned).
            if (im.swapchain_binding_.has_value() &&
                im.swapchain_binding_->scene_id == p.scene_id)
            {
                im.swapchain_binding_.reset();
            }
        }

        void handleAddView(Ctx& ctx, const AddViewPayload& p)
        {
            auto& im = impl(ctx);
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (!sc)
            {
                std::cerr << "[RenderServer] AddView failed: invalid scene_id="
                          << p.scene_id.index << "\n";
                replyToCurrent<AddViewPayload>(ctx, ViewCreatedReply{ViewHandle{}});
                return;
            }
            ViewCreateInfo ci{
                .initial_extent = p.extent,
                .debug_name     = p.name,
            };
            ViewHandle handle = sc->addView(ci);

            if (!handle.valid())
            {
                std::cerr << "[RenderServer] AddView failed in scene addView() for scene_id="
                          << p.scene_id.index << "\n";
                replyToCurrent<AddViewPayload>(ctx, ViewCreatedReply{ViewHandle{}});
                return;
            }

            // All views created via protocol are offscreen
            setupOffscreenViewTarget(im, sc, p.scene_id, handle, p.extent);

            // (Initial camera removed from AddView — View 去 3D 化: the client sends a
            // StandardViewCamera op for this view after addView. AddView is neutral.)

            replyToCurrent<AddViewPayload>(ctx, ViewCreatedReply{handle});
        }

        void handleRemoveView(Ctx& ctx, const RemoveViewPayload& p)
        {
            auto& im = impl(ctx);
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (sc) sc->removeView(p.view);

            // Remove from offscreen list (swap-and-pop) — pool destroyed via unique_ptr
            auto it = std::find_if(im.offscreen_views_.begin(), im.offscreen_views_.end(),
                [&](const auto& e) { return e.scene_id == p.scene_id && e.view_id == p.view; });
            if (it != im.offscreen_views_.end())
            {
                *it = std::move(im.offscreen_views_.back());
                im.offscreen_views_.pop_back();
            }

            // Clean up swapchain binding if it referenced this view
            if (im.swapchain_binding_.has_value() &&
                im.swapchain_binding_->view_id == p.view &&
                im.swapchain_binding_->scene_id == p.scene_id)
            {
                im.swapchain_binding_.reset();
            }
        }

        void handleResizeView(Ctx& ctx, const ResizeViewPayload& p)
        {
            auto& im = impl(ctx);
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (!sc) return;
            if (auto* v = sc->getView(p.view))
            {
                auto new_extent = p.new_extent;
                if (new_extent.width == 0 || new_extent.height == 0)
                    return;

                const auto& limits = im.res_ctx_->deviceContext()
                    .physicalDevice().properties().properties.limits;
                const uint32_t max_w = std::min(limits.maxFramebufferWidth, limits.maxImageDimension2D);
                const uint32_t max_h = std::min(limits.maxFramebufferHeight, limits.maxImageDimension2D);

                new_extent.width = std::clamp(new_extent.width, 1u, max_w);
                new_extent.height = std::clamp(new_extent.height, 1u, max_h);
                sc->resizeView(*v, new_extent);
            }
        }

        void handleSetSceneTime(Ctx& ctx, const SetSceneTimePayload& p)
        {
            auto& im = impl(ctx);
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (sc) sc->setSceneTime(p.total_time, p.delta_time, p.frame_number);
        }

        Expected<void> bindSwapchainInternal(
            GeneralRenderServer::Impl& im,
            RenderSceneId scene_id,
            ViewHandle view,
            const RenderTargetLayout& layout,
            bool replace_existing)
        {
            if (im.swapchain_binding_.has_value())
            {
                if (!replace_existing)
                    return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));
                im.swapchain_binding_.reset();
            }

            if (!im.swapchain_provider_)
                return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));

            auto* scene = im.renderer_->getScene(scene_id);
            if (!scene || !scene->getView(view))
                return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));

            scene->compileGraphTemplate(layout);

            im.swapchain_binding_ = GeneralRenderServer::Impl::SwapchainBinding{
                .scene_id = scene_id,
                .view_id  = view,
                .layout   = layout,
            };

            // Remove from offscreen_views_ — the view is now swapchain-bound
            auto it = std::remove_if(im.offscreen_views_.begin(), im.offscreen_views_.end(),
                [&](const GeneralRenderServer::Impl::OffscreenViewEntry& e) {
                    return e.scene_id == scene_id && e.view_id == view;
                });
            im.offscreen_views_.erase(it, im.offscreen_views_.end());

            return {};
        }

        void handleBindSwapchain(Ctx& ctx, const BindSwapchainPayload& p)
        {
            auto& im = impl(ctx);
            const auto layout = im.swapchain_provider_ ? im.swapchain_provider_->layout()
                                                       : RenderTargetLayout{};
            (void)bindSwapchainInternal(im, p.scene_id, p.view, layout, /*replace_existing=*/true);
        }

        void handleRequestSwapchainScene(Ctx& ctx, const RequestSwapchainScenePayload& p)
        {
            auto& im = impl(ctx);
            SwapchainBoundReply reply{};

            if (!im.swapchain_provider_)
            {
                reply.status = 1;
                replyToCurrent<RequestSwapchainScenePayload>(ctx, reply);
                return;
            }

            auto* scene = im.renderer_->getScene(p.scene_id);
            if (!scene)
            {
                reply.status = 2;
                replyToCurrent<RequestSwapchainScenePayload>(ctx, reply);
                return;
            }

            // Idempotent: if this scene is already swapchain-bound, return the
            // existing view instead of creating and binding a second one. Repeated
            // calls (scene switching back and forth, client re-init) otherwise
            // accumulated orphaned views with their per-view GPU state, since
            // bindSwapchainInternal only resets the binding optional — it never
            // removes the previously bound view from its scene. (medium)
            if (im.swapchain_binding_.has_value() &&
                im.swapchain_binding_->scene_id == p.scene_id)
            {
                reply.view   = ViewHandle{im.swapchain_binding_->view_id};
                reply.status = 0;
                replyToCurrent<RequestSwapchainScenePayload>(ctx, reply);
                return;
            }

            // Rebinding a DIFFERENT scene: remove the previously bound view so it
            // does not leak. (medium)
            if (im.swapchain_binding_.has_value())
            {
                if (auto* prev = im.renderer_->getScene(im.swapchain_binding_->scene_id))
                    prev->removeView(im.swapchain_binding_->view_id);
            }

            auto extent = im.swapchain_provider_->extent();
            ViewCreateInfo ci{};
            ci.initial_extent = {extent.width, extent.height};
            ci.debug_name     = "SwapchainView";
            const ViewHandle view_id = scene->addView(ci);
            if (!view_id.valid())
            {
                std::cerr << "[RenderServer] RequestSwapchainScene: addView failed for scene_id="
                          << p.scene_id.index << "\n";
                reply.status = 3;   // view creation failed
                replyToCurrent<RequestSwapchainScenePayload>(ctx, reply);
                return;
            }

            auto layout = im.swapchain_provider_->layout();
            if (!bindSwapchainInternal(im, p.scene_id, view_id, layout, true))
            {
                // Bind failed — drop the just-created view and report the error so
                // the client does not drive a nonexistent view. (medium)
                scene->removeView(view_id);
                reply.status = 4;   // swapchain bind failed
                replyToCurrent<RequestSwapchainScenePayload>(ctx, reply);
                return;
            }

            reply.view   = view_id;
            reply.status = 0;
            replyToCurrent<RequestSwapchainScenePayload>(ctx, reply);
        }

        void handlePick(Ctx& ctx, const PickPayload& /*p*/)
        {
            // TODO: integrate with picking system
            replyToCurrent<PickPayload>(ctx, PickResultReply{});
        }

        // Bytes-per-pixel for the offscreen color formats we read back.
        static uint32_t readbackBpp(VkFormat f) noexcept
        {
            switch (f)
            {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:       return 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
            case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
            default:                            return 0;
            }
        }

        // ── GPU->CPU offscreen-view readback (shared core) ───────────────────
        //
        // Records + submits a one-shot copy of a view's color image into a
        // caller-owned host buffer. The copy is self-contained (own staging
        // buffer + command buffer + fence); the sync handler waits the fence,
        // the async path polls it across ticks. Pixels are copied in the view's
        // NATIVE format (offscreen = BGRA8_SRGB). FIF slot 0 is read — the caller
        // is expected to have rendered identical content across all slots
        // (static asset / repeated frames). Same-queue submission orders the
        // copy after prior render submits, so it sees the last rendered content.

        // Record + submit the copy WITHOUT waiting. On success `j` holds the
        // in-flight buffer/fence/cb + dims; returns 0, else a non-zero status
        // (no GPU resources left allocated on failure).
        uint32_t submitReadbackCopy(GeneralRenderServer::Impl& im,
                                    GeneralRenderServer::Impl::PendingReadback& j)
        {
            OffscreenImagePool* pool = nullptr;
            for (auto& e : im.offscreen_views_)
            {
                if (e.scene_id == j.scene_id && e.view_id == j.view_id)
                {
                    pool = e.pool.get();
                    break;
                }
            }
            if (!pool) return 1;

            const RenderTargetLayout rt_layout = pool->layout();
            const TargetSlot slot = j.slot;                  // which output semantic to read (阶段4 P4c)
            if (!rt_layout.hasSlot(slot)) return 2;

            const RenderTargetSlotDesc& slot_desc = rt_layout.slot(slot);
            const VkFormat      fmt         = slot_desc.format;
            const VkImageLayout from_layout = slot_desc.final_layout;
            const uint32_t      bpp         = readbackBpp(fmt);
            const VkExtent2D    ext         = pool->extent();
            if (bpp == 0 || ext.width == 0 || ext.height == 0) return 3;

            const RenderTargetBinding& binding = pool->binding();
            const SlotImages& slot_imgs = binding.slot(slot);
            if (slot_imgs.images.empty()) return 4;
            const VkImage image = slot_imgs.images.front();

            const uint64_t needed = static_cast<uint64_t>(ext.width) * ext.height * bpp;
            if (needed > j.dst_capacity || j.dst_ptr == 0) return 5;

            // Host-visible, persistently-mapped staging buffer (raw VMA).
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size        = needed;
            bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                      | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocator      vma = im.dev_ctx_->vmaAllocator();
            VmaAllocationInfo stg_info{};
            if (vmaCreateBuffer(vma, &bci, &aci, &j.buf, &j.alloc, &stg_info) != VK_SUCCESS)
                return 6;
            j.mapped = stg_info.pMappedData;

            VkDevice      dev      = im.dev_ctx_->logicalDevice();
            VkCommandPool cmd_pool = im.res_ctx_->commandPool();

            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool        = cmd_pool;
            ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(dev, &ai, &j.cb) != VK_SUCCESS)
            {
                vmaDestroyBuffer(vma, j.buf, j.alloc); j.buf = VK_NULL_HANDLE;
                return 7;
            }

            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(j.cb, &bi);

            auto barrier = [&](VkImageLayout oldL, VkImageLayout newL,
                               VkAccessFlags srcA, VkAccessFlags dstA,
                               VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
            {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.oldLayout = oldL; b.newLayout = newL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                b.srcAccessMask = srcA; b.dstAccessMask = dstA;
                vkCmdPipelineBarrier(j.cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
            };

            barrier(from_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

            VkBufferImageCopy region{};
            region.bufferOffset      = 0;
            region.bufferRowLength   = 0; // tightly packed
            region.bufferImageHeight = 0;
            region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset       = {0, 0, 0};
            region.imageExtent       = {ext.width, ext.height, 1};
            vkCmdCopyImageToBuffer(j.cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   j.buf, 1, &region);

            barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, from_layout,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

            vkEndCommandBuffer(j.cb);

            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vkCreateFence(dev, &fci, nullptr, &j.fence);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers    = &j.cb;
            if (vkQueueSubmit(im.dev_ctx_->graphicsQueue(), 1, &si, j.fence) != VK_SUCCESS)
            {
                vkDestroyFence(dev, j.fence, nullptr); j.fence = VK_NULL_HANDLE;
                vkFreeCommandBuffers(dev, cmd_pool, 1, &j.cb); j.cb = VK_NULL_HANDLE;
                vmaDestroyBuffer(vma, j.buf, j.alloc); j.buf = VK_NULL_HANDLE;
                return 8;
            }

            j.width  = ext.width;
            j.height = ext.height;
            j.bpp    = bpp;
            j.format = fmt;
            j.needed = needed;
            return 0;
        }

        // Free GPU resources without copying (error / timeout paths).
        void freeReadbackGpu(GeneralRenderServer::Impl& im,
                             GeneralRenderServer::Impl::PendingReadback& j)
        {
            VkDevice dev = im.dev_ctx_->logicalDevice();
            if (j.fence) { vkDestroyFence(dev, j.fence, nullptr); j.fence = VK_NULL_HANDLE; }
            if (j.cb)    { vkFreeCommandBuffers(dev, im.res_ctx_->commandPool(), 1, &j.cb); j.cb = VK_NULL_HANDLE; }
            if (j.buf)   { vmaDestroyBuffer(im.dev_ctx_->vmaAllocator(), j.buf, j.alloc); j.buf = VK_NULL_HANDLE; }
        }

        // Fence has signaled: make GPU writes visible, copy pixels out, free GPU
        // resources, and fill `reply`. Assumes submitReadbackCopy succeeded.
        void finishReadbackCopy(GeneralRenderServer::Impl& im,
                                GeneralRenderServer::Impl::PendingReadback& j,
                                ReadbackViewReply& reply)
        {
            VmaAllocator vma = im.dev_ctx_->vmaAllocator();
            vmaInvalidateAllocation(vma, j.alloc, 0, j.needed);
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(j.dst_ptr)),
                        j.mapped, static_cast<std::size_t>(j.needed));

            reply.status          = 0;
            reply.width           = j.width;
            reply.height          = j.height;
            reply.bytes_per_pixel = j.bpp;
            reply.bytes_written   = j.needed;
            reply.format          = static_cast<uint32_t>(j.format);

            freeReadbackGpu(im, j);
        }

        // Synchronous readback: submit + wait + finish, reply in the current
        // request's frame. Contract: the view must already be rendered + not
        // rendered concurrently (use between ticks / before recording a frame).
        void handleReadbackView(Ctx& ctx, const ReadbackViewPayload& p)
        {
            auto& im = impl(ctx);
            GeneralRenderServer::Impl::PendingReadback j{};
            j.scene_id     = p.scene_id;
            j.view_id      = p.view;
            j.dst_ptr      = p.dst_ptr;
            j.dst_capacity = p.dst_capacity;
            j.slot         = static_cast<TargetSlot>(p.slot);

            ReadbackViewReply reply{};
            const uint32_t st = submitReadbackCopy(im, j);
            if (st != 0) { reply.status = st; replyToCurrent<ReadbackViewPayload>(ctx, reply); return; }

            // Finite timeout: a GPU stall must never wedge the server thread.
            constexpr uint64_t kReadbackFenceTimeoutNs = 5'000'000'000ull; // 5 s
            const VkResult wres = vkWaitForFences(im.dev_ctx_->logicalDevice(),
                                                  1, &j.fence, VK_TRUE, kReadbackFenceTimeoutNs);
            if (wres != VK_SUCCESS)  // VK_TIMEOUT or device-lost: do not hang
            {
                freeReadbackGpu(im, j);
                reply.status = 9; replyToCurrent<ReadbackViewPayload>(ctx, reply); return;
            }

            finishReadbackCopy(im, j, reply);
            replyToCurrent<ReadbackViewPayload>(ctx, reply);
        }

        // Asynchronous readback: enqueue a deferred job and return WITHOUT a
        // reply. pollPendingReadbacks() settles `settle_frames` ticks, submits
        // the copy, polls the fence, and sends the deferred reply by request_id.
        // The client never blocks the calling (UI) thread.
        void handleReadbackViewAsync(Ctx& ctx, const ReadbackViewAsyncPayload& p)
        {
            auto& im = impl(ctx);
            GeneralRenderServer::Impl::PendingReadback j{};
            j.scene_id     = p.scene_id;
            j.view_id      = p.view;
            j.dst_ptr      = p.dst_ptr;
            j.dst_capacity = p.dst_capacity;
            j.slot         = static_cast<TargetSlot>(p.slot);
            j.request_id   = ctx.currentRequestId();
            j.settle_left  = std::max<uint32_t>(p.settle_frames, im.frames_in_flight_);
            j.deadline     = 600; // ticks before a stuck fence is declared failed
            if (p.dst_ptr == 0) { j.done = true; j.reply.status = 5; } // bad dst
            im.pending_readbacks_.push_back(j);
            // Deferred — reply is sent later by pollPendingReadbacks().
        }

        // Per-tick state machine for the in-flight async readbacks: settle ->
        // submit -> poll fence -> finish. Marks `done` + fills `reply` when an
        // entry resolves; the reply itself is sent by pollPendingReadbacks().
        void advancePendingReadbacks(GeneralRenderServer::Impl& im)
        {
            if (im.pending_readbacks_.empty()) return;
            VkDevice dev = im.dev_ctx_->logicalDevice();
            for (auto& j : im.pending_readbacks_)
            {
                if (j.done) continue;
                if (!j.submitted)
                {
                    if (j.settle_left > 0) { --j.settle_left; continue; }
                    const uint32_t st = submitReadbackCopy(im, j);
                    j.submitted = true;
                    if (st != 0) { j.reply.status = st; j.done = true; }
                    continue; // poll the fence on a later tick
                }
                const VkResult fs = vkGetFenceStatus(dev, j.fence);
                if (fs == VK_NOT_READY)
                {
                    if (j.deadline > 0) --j.deadline;
                    if (j.deadline == 0) { freeReadbackGpu(im, j); j.reply.status = 9; j.done = true; }
                    continue;
                }
                if (fs == VK_SUCCESS) finishReadbackCopy(im, j, j.reply);
                else { freeReadbackGpu(im, j); j.reply.status = 10; } // device lost
                j.done = true;
            }
        }

        // ── Scene activation / bulk-data context ─────────────────────────────

        void handleSetActiveScene(Ctx& ctx, const SetActiveScenePayload& p)
        {
            auto& im = impl(ctx);
            im.current_bulk_scene_ = p.scene_id;
            replyToCurrent<SetActiveScenePayload>(ctx, GenericOkReply{0});
        }

        // (concatMeshLods / buildMeshSectionRecords / writeLocalBoundsFromMesh moved to
        //  the StandardMeshStack feature — MeshStackOperationHandlers.cpp (Stage C).)

        // The mesh-instance lifecycle handlers (AddMeshInstance / RemoveMeshInstance
        // / Make|HideInstanceForView / UpdateInstanceFlags|RenderState|UserMeta +
        // the TransformBatch bulk handler) AND their assembly bodies live in the
        // StandardMeshStack feature: renderer/features/meshstack/MeshStackOperationHandlers.cpp
        // (Stage C). The core only still exports the generic lookupScene /
        // lookupCurrentBulkScene scene-resolvers; the dispatcher registers no mesh op.

        // ── The stateless resource & feature-lifecycle protocol handlers moved to a
        //    sibling TU: RenderServerHandlers.cpp (registerResourceAndFeatureHandlers).
        //    That covers RegisterFeatureType / UnregisterFeatureType / QueryTypeId /
        //    AddFeature / RemoveFeature / SetFeatureEnabled / DumpRenderGraph /
        //    QueryFeatureParams + the texture & shader handlers — they need nothing from
        //    this TU's GPU-target machinery. The scene / view / swapchain / readback /
        //    pick handlers stay here, next to the machinery they share with tick().

        // ── Resource handlers ────────────────────────────────────────────────

        // (handleUploadMesh / handleDestroyMesh AND their async assembly live in the
        //  StandardMeshStack feature: MeshStackOperationHandlers.cpp (Stage C), dynamic
        //  ids via register_ops_fn. The core dispatcher registers no mesh op. The upload
        //  REPLY (MeshUploadedReply) is still emitted by the shared async-transfer worker
        //  below — that is core shared infra (mesh + texture), not a mesh op.)

        // (GPU skinning — applyBoneSkinningOne + the UploadBonePalette/UploadBoneBatch
        //  handlers — moved to SkinningFeature: SkinningOperationHandlers.cpp. They are
        //  registered with dynamic TypeIds via the feature's register_ops_fn, so the
        //  core server no longer dispatches skinning.)

        // (handleCreateTexture2D / handleCreateCubeTexture / handleUpdateTexture2D /
        //  handleUpdateCubeTexture / handleDestroyTexture / handleDestroyCubeTexture moved
        //  to RenderServerHandlers.cpp.)

        // The material upload / modify / destroy handlers AND their assembly bodies live
        // in the StandardMaterial feature: renderer/features/material/MaterialOperationHandlers.cpp
        // (Stage C). The core dispatcher registers no material op. (handleUploadMaterial /
        // handleModifyMaterial — the builtin closure-material handlers — were retired in W5a.)

        // (handleCompileShader / handleDestroyShader moved to RenderServerHandlers.cpp.)

        // (Light create/update/destroy/batch handlers moved to LightFeature:
        //  renderer/features/light/LightOperationHandlers.cpp. They register with
        //  DYNAMIC TypeIds via register_ops_fn; the core server no longer dispatches
        //  light. createLight inlines the old GeneralRenderServer::createLight there.)

        // (Point cloud handlers moved to PCOperationHandlers.cpp)
        // (Trajectory handlers moved to TrajectoryOperationHandlers.cpp)

        // ── Bulk data handlers ───────────────────────────────────────────────

        // handleTransformBatch (per-frame instance transforms) moved to the
        // StandardMeshStack feature (MeshStackOperationHandlers.cpp); it resolves
        // the active scene via the exported lookupCurrentBulkScene shim.

        // (handleViewFrameUpdate removed — View 去 3D 化: the per-view camera update is a
        // StandardViewCamera feature-scoped op now, handled in ViewCameraOperationHandlers.cpp.
        // The core dispatcher no longer registers it.)

    } // anonymous namespace

    // ─────────────────────────────────────────────────────────────────────
    //  Registration
    // ─────────────────────────────────────────────────────────────────────

    // Defined in RenderServerHandlers.cpp — registers the texture/shader + feature-lifecycle
    // protocol handlers (split out to keep this TU to the server's lifecycle/frame loop).
    void registerResourceAndFeatureHandlers(GeneralRenderServer::Dispatcher& d);

    static void registerServerHandlers(GeneralRenderServer::Dispatcher& d)
    {
        // ── CommandOp: Scene lifecycle ──
        d.registerUnary<CreateScenePayload,                 &handleCreateScene>      (opcodes::CommandOp, type_ids::CreateScene,       "CreateScene");
        d.registerUnary<DestroyScenePayload,                &handleDestroyScene>     (opcodes::CommandOp, type_ids::DestroyScene,      "DestroyScene");
        d.registerUnary<AddViewPayload,                     &handleAddView>          (opcodes::CommandOp, type_ids::AddView,           "AddView");
        d.registerUnary<RemoveViewPayload,                  &handleRemoveView>       (opcodes::CommandOp, type_ids::RemoveView,        "RemoveView");
        d.registerUnary<ResizeViewPayload,                  &handleResizeView>       (opcodes::CommandOp, type_ids::ResizeView,        "ResizeView");
        d.registerUnary<SetSceneTimePayload,                &handleSetSceneTime>     (opcodes::CommandOp, type_ids::SetSceneTime,      "SetSceneTime");
        d.registerUnary<PickPayload,                        &handlePick>             (opcodes::CommandOp, type_ids::Pick,              "Pick");
        d.registerUnary<ReadbackViewPayload,                &handleReadbackView>     (opcodes::CommandOp, type_ids::ReadbackView,      "ReadbackView");
        d.registerUnary<ReadbackViewAsyncPayload,           &handleReadbackViewAsync>(opcodes::CommandOp, type_ids::ReadbackViewAsync, "ReadbackViewAsync");
        // Mesh-instance ops (Add/Remove/Make|Hide/UpdateFlags/RenderState/UserMeta)
        // are registered dynamically by the StandardMeshStack feature's
        // register_ops_fn — no longer static core handlers.
        d.registerUnary<SetActiveScenePayload,              &handleSetActiveScene>   (opcodes::CommandOp, type_ids::SetActiveScene,    "SetActiveScene");
        d.registerUnary<BindSwapchainPayload,               &handleBindSwapchain>    (opcodes::CommandOp, type_ids::BindSwapchain,     "BindSwapchain");
        d.registerUnary<RequestSwapchainScenePayload,       &handleRequestSwapchainScene>(opcodes::CommandOp, type_ids::RequestSwapchainScene, "RequestSwapchainScene");
        // ── Resource (texture/shader) + feature-lifecycle handlers ──
        // These are stateless protocol→resource delegations with no GPU-target machinery,
        // so they live in a sibling TU (RenderServerHandlers.cpp) to keep this file to the
        // server object's lifecycle / frame loop / GPU-target handlers.
        registerResourceAndFeatureHandlers(d);
        // (Mesh/material/light/skinning/point-cloud/trajectory ops + TransformBatch are all
        //  registered dynamically via each feature's register_ops_fn, not here.)
        // ── BulkData ──
        // (TransformBatch now registered dynamically via StandardMeshStack::register_ops_fn)
        // (ViewFrameUpdate removed — per-view camera is the StandardViewCamera feature op now.)
        // (Light batch now registered dynamically via LightFeature::register_ops_fn)
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Construction / tick / attachToWindow
    // ─────────────────────────────────────────────────────────────────────

    GeneralRenderServer::GeneralRenderServer(
        std::shared_ptr<Channel> channel,
        std::shared_ptr<RenderChannelSync> sync)
        : GeneralRenderServer(std::move(channel), std::move(sync),
                              std::make_unique<Impl>())
    {
    }

    GeneralRenderServer::GeneralRenderServer(
        std::shared_ptr<Channel> channel,
        std::shared_ptr<RenderChannelSync> sync,
        std::unique_ptr<Impl> impl)
        : RenderServer<>(std::move(channel), std::move(sync), impl->dispatcher)
        , impl_(std::move(impl))
    {
        impl_->server_ = this;
    }

    GeneralRenderServer::~GeneralRenderServer() = default;

    Expected<void> GeneralRenderServer::init(ServerConfig config)
    {
        auto result = impl_->init(std::move(config));
        if (!result) return result;
        registerServerHandlers(impl_->dispatcher);
        return {};
    }

    bool GeneralRenderServer::drainRequest()
    {
        if (!acquireAndExecute(/*blocking=*/false, impl_.get())) return false;
        impl_->processUploadCompletions(replyBuilder());
        return finalizeReplies(/*blocking=*/false);
    }

    bool GeneralRenderServer::drainRequestBlocking()
    {
        if (!acquireAndExecute(/*blocking=*/true, impl_.get())) return false;
        impl_->processUploadCompletions(replyBuilder());
        return finalizeReplies(/*blocking=*/true);
    }

    // ── Per-kind completion finalization ──────────────────────────────────

    void GeneralRenderServer::Impl::finalizeMeshCompletion(
        TransferCompletion& c, bool needs_qfot,
        uint32_t src_family, uint32_t dst_family)
    {
        auto* mr = render_ctx_->globalRegistry().find<MeshResources>();
        if (!mr) return;

        if (c.timeline_value == 0)
        {
            MeshResources::PendingStagingCopy sc{};
            sc.stg_buf        = c.stg_buf;
            sc.vbo_dst        = c.mesh.vbo_buf;
            sc.vbo_stg_offset = 0;
            sc.vbo_dst_offset = c.mesh.vbo_offset;
            sc.vbo_size       = c.mesh.vbo_size;
            sc.ibo_dst        = c.mesh.ibo_buf;
            sc.ibo_stg_offset = c.mesh.vbo_size;
            sc.ibo_dst_offset = c.mesh.ibo_offset;
            sc.ibo_size       = c.mesh.ibo_size;
            sc.mesh_index     = c.mesh.mesh_index;
            mr->pushStagingCopy(sc);
        }
        else
        {
            if (needs_qfot)
            {
                mr->pushAcquireBarrier(c.mesh.vbo_buf, c.mesh.vbo_offset,
                                       c.mesh.vbo_size, src_family, dst_family);
                if (c.mesh.ibo_size > 0)
                    mr->pushAcquireBarrier(c.mesh.ibo_buf, c.mesh.ibo_offset,
                                           c.mesh.ibo_size, src_family, dst_family);
            }
            mr->markReady(c.mesh.mesh_index);
        }
    }

    void GeneralRenderServer::Impl::finalizeTexture2DCompletion(
        TransferCompletion& c, bool needs_qfot,
        uint32_t src_family, uint32_t dst_family)
    {
        auto* tr = render_ctx_->globalRegistry().find<TextureResources>();
        if (!tr) return;

        auto& bcs = tr->bindlessSet2D();
        if (c.timeline_value == 0)
        {
            bcs.finalizeTransferredTexture(
                c.texture.slot_index, c.texture.image, c.texture.image_alloc,
                c.texture.view, c.texture.sampler, c.texture.format,
                c.texture.mip_levels, c.texture.array_layers,
                c.texture.width, c.texture.height);
            BindlessCombinedSet::PendingStagingTexture st{};
            st.stg_buf     = c.stg_buf;
            st.stg_size    = c.stg_size;
            st.slot_index  = c.texture.slot_index;
            st.do_mips     = c.texture.needs_mip_gen;
            st.is_cube     = false;
            st.face_stride = 0;
            st.mip_copy_count = std::clamp<uint32_t>(
                c.texture.uploaded_mip_count,
                1u,
                rdesc::kTextureMaxMipCount);
            for (uint32_t i = 0; i < st.mip_copy_count; ++i)
            {
                st.mip_copies[i].buffer_offset = c.texture.uploaded_mips[i].buffer_offset;
                st.mip_copies[i].mip_level = c.texture.uploaded_mips[i].mip_level;
                st.mip_copies[i].width = c.texture.uploaded_mips[i].width;
                st.mip_copies[i].height = c.texture.uploaded_mips[i].height;
            }
            bcs.pushStagingTextureCopy(st);
        }
        else
        {
            if (needs_qfot)
            {
                bcs.pushImageAcquireBarrier(
                    c.texture.image, c.texture.mip_levels, c.texture.array_layers,
                    c.texture.needs_mip_gen
                        ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    src_family, dst_family);
            }
            if (c.texture.needs_mip_gen)
                bcs.pushDeferredMipGen(c.texture.slot_index);
            bcs.finalizeTransferredTexture(
                c.texture.slot_index, c.texture.image, c.texture.image_alloc,
                c.texture.view, c.texture.sampler, c.texture.format,
                c.texture.mip_levels, c.texture.array_layers,
                c.texture.width, c.texture.height);
        }
    }

    void GeneralRenderServer::Impl::finalizeTextureCubeCompletion(
        TransferCompletion& c, bool needs_qfot,
        uint32_t src_family, uint32_t dst_family)
    {
        auto* tr = render_ctx_->globalRegistry().find<TextureResources>();
        if (!tr) return;

        auto& bcs = tr->bindlessSetCube();
        if (c.timeline_value == 0)
        {
            bcs.finalizeTransferredTexture(
                c.texture.slot_index, c.texture.image, c.texture.image_alloc,
                c.texture.view, c.texture.sampler, c.texture.format,
                c.texture.mip_levels, c.texture.array_layers,
                c.texture.width, c.texture.height);
            BindlessCombinedSet::PendingStagingTexture st{};
            st.stg_buf     = c.stg_buf;
            st.stg_size    = c.stg_size;
            st.slot_index  = c.texture.slot_index;
            st.do_mips     = false;
            st.is_cube     = true;
            st.face_stride = static_cast<VkDeviceSize>(c.texture.width)
                           * c.texture.height * 4;
            bcs.pushStagingTextureCopy(st);
        }
        else
        {
            if (needs_qfot)
            {
                bcs.pushImageAcquireBarrier(
                    c.texture.image, c.texture.mip_levels, c.texture.array_layers,
                    c.texture.needs_mip_gen
                        ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    src_family, dst_family);
            }
            bcs.finalizeTransferredTexture(
                c.texture.slot_index, c.texture.image, c.texture.image_alloc,
                c.texture.view, c.texture.sampler, c.texture.format,
                c.texture.mip_levels, c.texture.array_layers,
                c.texture.width, c.texture.height);
        }
    }

    // ── Dispatch table for completion finalization ────────────────────────

    void GeneralRenderServer::Impl::finalizeCompletion(
        TransferCompletion& c, bool needs_qfot,
        uint32_t src_family, uint32_t dst_family)
    {
        using Kind = TransferCompletion::Kind;
        using FinalizeFn = void (Impl::*)(TransferCompletion&, bool, uint32_t, uint32_t);

        static constexpr FinalizeFn kDispatch[] = {
            &Impl::finalizeMeshCompletion,       // Kind::MeshBuffer  = 0
            &Impl::finalizeTexture2DCompletion,   // Kind::Texture2D   = 1
            &Impl::finalizeTextureCubeCompletion,  // Kind::TextureCube = 2
        };
        static_assert(static_cast<int>(Kind::MeshBuffer)  == 0);
        static_assert(static_cast<int>(Kind::Texture2D)   == 1);
        static_assert(static_cast<int>(Kind::TextureCube)  == 2);

        (this->*kDispatch[static_cast<int>(c.kind)])(c, needs_qfot, src_family, dst_family);

        if (c.request_id != UINT32_MAX)
            pending_deferred_replies_.push_back({c.request_id, c.kind, c.resource_handle, c.resource_gen});

        if (c.stg_buf != VK_NULL_HANDLE)
            staging_pending_this_tick_.emplace_back(dev_ctx_->vmaAllocator(), c.stg_buf, c.stg_alloc);
    }

    // ── processUploadCompletions ──────────────────────────────────────────

    void GeneralRenderServer::Impl::processUploadCompletions(FrameReplyBuilder<64>& replies)
    {
        // 0. Submit every transfer command buffer the upload workers recorded
        //    since the last tick. The render thread is the SOLE submitter to the
        //    Transfer Queue (workers only record now), so this collides with
        //    nothing — the frame submit is on this same thread. Removes the
        //    cross-thread vkQueueSubmit2 race without any queue lock. The just-
        //    submitted completions haven't retired yet, so defer them into
        //    pending_completions_ exactly like freshly drained ones; step 3
        //    re-checks them against the timeline and finalizes once retired.
        if (upload_pool_)
        {
            const uint32_t fn = upload_pool_->flushPendingSubmits(completion_buf_, kMaxDrainBatch);
            for (uint32_t i = 0; i < fn; ++i)
                pending_completions_.push_back(completion_buf_[i]);
        }

        // 1. Flush deferred replies accumulated during previous ticks.
        for (auto& dr : pending_deferred_replies_)
        {
            using Kind = TransferCompletion::Kind;
            switch (dr.kind)
            {
            case Kind::MeshBuffer:
                replies.push<MeshUploadedReply>(
                    type_ids::ReplyMeshUploaded,
                    MeshUploadedReply{RMeshHandle{dr.resource_index, dr.resource_gen}, 0u},
                    0, dr.request_id);
                break;
            case Kind::Texture2D:
                replies.push<Texture2DCreatedReply>(
                    type_ids::ReplyTexture2DCreated,
                    Texture2DCreatedReply{RTextureHandle{dr.resource_index, dr.resource_gen}, 0u},
                    0, dr.request_id);
                break;
            case Kind::TextureCube:
                replies.push<CubeTextureCreatedReply>(
                    type_ids::ReplyCubeTextureCreated,
                    CubeTextureCreatedReply{RTextureHandle{dr.resource_index, dr.resource_gen}, 0u},
                    0, dr.request_id);
                break;
            }
        }
            
        pending_deferred_replies_.clear();

        if (!upload_pool_) return;

        // 2. Query GPU timeline value (non-blocking).
        uint64_t gpu_value = 0;
        const VkResult sem_res = vkGetSemaphoreCounterValue(
            dev_ctx_->logicalDevice().handle(),
            upload_pool_->timelineSemaphore(),
            &gpu_value
        );
        if (sem_res != VK_SUCCESS)
        {
            // Don't silently treat the result as gpu_value==0 — that re-defers every
            // pending completion forever and never surfaces the failure, so client
            // upload futures hang. Log once, then: on a transient error skip
            // finalization this tick and retry next tick; on device loss the
            // completions can never finalize, so fail all pending ones with an error
            // reply (status!=0) to unblock the waiting clients. (M2)
            static bool s_logged_sem_fail = false;
            if (!s_logged_sem_fail)
            {
                std::cerr << "[RenderServer] vkGetSemaphoreCounterValue failed: "
                          << sem_res << " — upload completions deferred\n";
                s_logged_sem_fail = true;
            }
            if (sem_res == VK_ERROR_DEVICE_LOST)
            {
                using Kind = TransferCompletion::Kind;
                for (auto& c : pending_completions_)
                {
                    if (c.request_id == UINT32_MAX) continue;
                    const RTextureHandle th{c.resource_handle, c.resource_gen};
                    switch (c.kind)
                    {
                    case Kind::MeshBuffer:
                        replies.push<MeshUploadedReply>(type_ids::ReplyMeshUploaded,
                            MeshUploadedReply{RMeshHandle{c.resource_handle, c.resource_gen}, 1u},
                            0, c.request_id);
                        break;
                    case Kind::Texture2D:
                        replies.push<Texture2DCreatedReply>(type_ids::ReplyTexture2DCreated,
                            Texture2DCreatedReply{th, 1u}, 0, c.request_id);
                        break;
                    case Kind::TextureCube:
                        replies.push<CubeTextureCreatedReply>(type_ids::ReplyCubeTextureCreated,
                            CubeTextureCreatedReply{th, 1u}, 0, c.request_id);
                        break;
                    }
                }
                pending_completions_.clear();
            }
            return;
        }

        const bool     needs_qfot = upload_pool_->hasTransferQueue();
        const uint32_t src_family = upload_pool_->transferFamily();
        const uint32_t dst_family = upload_pool_->graphicsFamily();

        // 3. Re-check previously deferred completions.
        auto new_end = std::remove_if(
            pending_completions_.begin(), pending_completions_.end(),
            [&](TransferCompletion& c)
            {
                if (c.timeline_value <= gpu_value)
                {
                    finalizeCompletion(c, needs_qfot, src_family, dst_family);
                    return true; // remove from pending
                }
                return false;
            }
        );
        pending_completions_.erase(new_end, pending_completions_.end());

        // 4. Drain new completions from MPSC ring.
        const uint32_t n = upload_pool_->drainCompletions(completion_buf_, kMaxDrainBatch);
        for (uint32_t i = 0; i < n; ++i)
        {
            auto& c = completion_buf_[i];
            if (c.timeline_value == 0 || c.timeline_value <= gpu_value)
                finalizeCompletion(c, needs_qfot, src_family, dst_family);
            else
                pending_completions_.push_back(c);
        }
    }

    Expected<void> GeneralRenderServer::attachToWindow(lux::window::LuxWindow& window)
    {
        impl_->attached_window_ = &window;
        if (!impl_->surface_.init(window, impl_->inst_ctx_->instance()))
            return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));
        
        const auto fb = window.framebufferSize();

        SwapchainProvider::Config sc_cfg{};
        sc_cfg.width            = (fb.width  > 0) ? fb.width  : 1u;
        sc_cfg.height           = (fb.height > 0) ? fb.height : 1u;
        sc_cfg.enable_vsync     = impl_->enable_vsync_;

        auto swapchain = SwapchainProvider::create(
            *impl_->res_ctx_, impl_->surface_, sc_cfg
        );
        if (!swapchain)
            return lux::cxx::unexpected(make_error_code(ERenderError::SwapchainCreateFailed));
        impl_->swapchain_provider_ = std::make_unique<SwapchainProvider>(std::move(*swapchain));
        impl_->cached_swapchain_layout_ = impl_->swapchain_provider_->layout();

        // Create FrameDriver now that we have a valid ResourceContext
        impl_->frame_driver_ = std::make_unique<FrameDriver>(
            *impl_->res_ctx_, impl_->frames_in_flight_
        );

        return {};
    }

    // ── Standalone deferred-reply flush ────────────────────────────────
    void GeneralRenderServer::flushDeferredRepliesOnly()
    {
        auto& im = *impl_;

        if (im.pending_deferred_replies_.empty()) return;

        auto* slot = channel().responses.tryBeginWrite();
        if (!slot) return;

        FrameReplyBuilder<64> builder(*slot);
        builder.begin();  // completion-only frame (no matching request)
        for (auto& dr : im.pending_deferred_replies_)
        {
            using Kind = TransferCompletion::Kind;
            switch (dr.kind)
            {
            case Kind::MeshBuffer:
                builder.push<MeshUploadedReply>(
                    type_ids::ReplyMeshUploaded,
                    MeshUploadedReply{RMeshHandle{dr.resource_index, dr.resource_gen}, 0u},
                    0, dr.request_id);
                break;
            case Kind::Texture2D:
                builder.push<Texture2DCreatedReply>(
                    type_ids::ReplyTexture2DCreated,
                    Texture2DCreatedReply{RTextureHandle{dr.resource_index, dr.resource_gen}, 0u},
                    0, dr.request_id);
                break;
            case Kind::TextureCube:
                builder.push<CubeTextureCreatedReply>(
                    type_ids::ReplyCubeTextureCreated,
                    CubeTextureCreatedReply{RTextureHandle{dr.resource_index, dr.resource_gen}, 0u},
                    0, dr.request_id);
                break;
            }
        }
            
        if (!channel().responses.publishWrite()){
            return;
        }
        im.pending_deferred_replies_.clear();
        channelSync().notifyReplyStateChanged();
    }

    // Advance + flush in-flight async readbacks (ReadbackViewAsync). The GPU
    // state machine (settle/submit/poll/finish) runs in advancePendingReadbacks;
    // here we send the deferred replies (by request_id) for resolved entries and
    // drop them. Ring-full / closed reply channel → retry next tick.
    void GeneralRenderServer::pollPendingReadbacks()
    {
        auto& im = *impl_;
        advancePendingReadbacks(im);

        bool any_done = false;
        for (auto& j : im.pending_readbacks_)
            if (j.done) { any_done = true; break; }
        if (!any_done) return;

        auto* slot = channel().responses.tryBeginWrite();
        if (!slot) return; // ring full — retry next tick (entries stay done)

        FrameReplyBuilder<64> builder(*slot);
        builder.begin();
        for (auto& j : im.pending_readbacks_)
            if (j.done)
                builder.push<ReadbackViewReply>(
                    type_ids::ReplyReadbackView, j.reply, 0, j.request_id);

        if (!channel().responses.publishWrite()) return; // retry next tick
        channelSync().notifyReplyStateChanged();

        im.pending_readbacks_.erase(
            std::remove_if(im.pending_readbacks_.begin(), im.pending_readbacks_.end(),
                           [](const Impl::PendingReadback& j) { return j.done; }),
            im.pending_readbacks_.end());
    }

    // ── tick ─────────────────────────────────────────────────────────────

    void GeneralRenderServer::Impl::beginTickFrame()
    {
        // FrameDriver::beginFrame() has already waited this slot's fence.
        frame_orchestrator_.beginFrame(*renderer_);
        async_deferred_staging_[current_stamp_.slotIndex()].clear();
        // This slot's fence is now waited → the offscreen pools retired into it (by a
        // DestroyScene fif frames ago) are GPU-idle and safe to destroy. (P0-3)
        async_deferred_pools_[current_stamp_.slotIndex()].clear();

        VRAMBudgetGuard budget(dev_ctx_->vmaAllocator());
        auto snap = budget.snapshot();
        if (snap.nearFull(0.95f))
            expansion_suppressed_ = true;
        else if (!snap.nearFull(0.85f))
            expansion_suppressed_ = false;
    }

    void GeneralRenderServer::Impl::endTickFrame(bool uploads_recorded)
    {
        // Move staging buffers accumulated during drain into the FIF ring now,
        // after runUploadPhase() has recorded all copies that read them.
        // beginTickFrame()'s .clear() fires only at next reuse of this slot
        // (after the slot fence is waited), so the GPU has finished by then.
        // Skip on upload-less ticks: StagingOnly copy records are still queued,
        // so the buffers must outlive the future tick that records them.
        if (uploads_recorded)
        {
            auto& ring_slot = async_deferred_staging_[current_stamp_.slotIndex()];
            for (auto& sb : staging_pending_this_tick_)
                ring_slot.push_back(std::move(sb));
            staging_pending_this_tick_.clear();
        }

        frame_orchestrator_.endFrame(*renderer_);
    }

    bool GeneralRenderServer::tick()
    {
        auto& im = *impl_;

        // Generate the authoritative frame stamp BEFORE draining requests,
        // so that handlers see the correct FIF slot index.
        // image_index is not yet known (swapchain acquire comes later);
        // it will be patched into rt.stamp after beginFrame().
        if (!RenderTickPipeline::runTick(
                im.frame_orchestrator_, im.current_stamp_,
                [&]() { return drainRequestBlocking(); }, 0
            ))
        {
            return false;
        }

        const auto& limits = im.res_ctx_->deviceContext()
            .physicalDevice().properties().properties.limits;
        const uint32_t max_w = std::min(limits.maxFramebufferWidth, limits.maxImageDimension2D);
        const uint32_t max_h = std::min(limits.maxFramebufferHeight, limits.maxImageDimension2D);
        auto sanitize_extent = [&](VkExtent2D ext) {
            ext.width = std::clamp(ext.width, 1u, max_w);
            ext.height = std::clamp(ext.height, 1u, max_h);
            return ext;
        };

        // Flush any pending deferred replies independently of new requests.
        flushDeferredRepliesOnly();

        if (im.offscreen_views_.empty() && !im.swapchain_binding_.has_value()) return true;

        // Handle pending offscreen resizes before beginning the frame.
        // Single-pass processing avoids duplicate scene/view lookups.
        if (im.frame_driver_)
        {
            for (auto& e : im.offscreen_views_)
            {
                auto* scene = im.renderer_->getScene(e.scene_id);
                auto* view = scene ? scene->getView(e.view_id) : nullptr;
                if (!view || !view->resize_pending)
                    continue;

                VkExtent2D ext = sanitize_extent({view->current_extent.width, view->current_extent.height});
                e.pool->resize(ext);
                view->resize_pending = false;
            }
        }

        const auto& stamp = im.current_stamp_;

        // Begin frame — wait fence, acquire swapchain if any, begin CB
        SwapchainProvider* sc_ptr = im.swapchain_provider_.get();
        FrameRuntime rt{};
        rt.stamp       = stamp;
        if (im.frame_driver_)
        {
            rt = im.frame_driver_->beginFrame(stamp.slotIndex(), stamp.serial, sc_ptr);
            im.frame_orchestrator_.patchImageIndex(rt.image_index);
            im.current_stamp_ = im.frame_orchestrator_.stamp();
            rt.stamp = im.current_stamp_;  // re-attach after FrameDriver fills the rest

            // Fence wait is completed in FrameDriver::beginFrame().
            // Only now is it safe to retire this slot's staging/upload state
            // and advance scene frame state.
            im.beginTickFrame();

            if (rt.primary_cmd == VK_NULL_HANDLE)
            {
                im.endTickFrame(false);
                return true;  // minimised / rebuild failed — skip frame
            }
        }
        else
        {
            // Offscreen-only path without FrameDriver still advances renderer
            // frame lifecycle, but has no swapchain/image acquire step.
            im.beginTickFrame();
        }

        // Set swapchain present_target if swapchain is active.
        // Declared at this scope so it outlives renderSingleView() below.
        RenderTargetBinding sc_binding;
        if (sc_ptr)
        {
            sc_binding = sc_ptr->makeFrameBinding(rt.image_index);
            rt.present_target = &sc_binding;
        }

        // Upload phase: global + per-scene transfers, once per tick.
        if (rt.primary_cmd != VK_NULL_HANDLE)
            im.renderer_->runUploadPhase(rt.primary_cmd);

        // 3. Render offscreen views
        // Build a compact scene/view batch to avoid per-frame hash maps. Reuse the
        // Impl member (cleared here) instead of stack-constructing 3 vectors per
        // tick; clear BEFORE the loop so no stale scene/view pointers survive. (P-5)
        SceneViewBatch& scene_view_batch = im.scene_view_batch_;
        scene_view_batch.clear();
        for (auto& e : im.offscreen_views_)
        {
            auto* scene = im.renderer_->getScene(e.scene_id);
            auto* view = scene ? scene->getView(e.view_id) : nullptr;
            if (!scene || !view)
                continue;

            scene_view_batch.add(scene, view);
            auto binding = e.pool->makeFrameBinding(stamp.slotIndex());
            const auto& item = scene_view_batch.items().back();
            // Headless tick without a FrameDriver leaves rt.primary_cmd null (the
            // upload phase above is already guarded for the same reason). Skip the
            // recording here instead of issuing vkCmdBeginRendering/barriers on a
            // null command buffer (crash/UB). (C-9)
            if (rt.primary_cmd != VK_NULL_HANDLE)
                im.renderer_->renderSingleView(
                    *item.scene, *item.view, binding, rt, item.cross_view_index);
        }

        // 4. Render swapchain view
        if (sc_ptr && im.swapchain_binding_.has_value())
        {
            auto& sb = *im.swapchain_binding_;
            auto* sc_scene = im.renderer_->getScene(sb.scene_id);
            auto* sv = sc_scene ? sc_scene->getView(sb.view_id) : nullptr;
            if (sc_scene && sv && rt.present_target && rt.primary_cmd != VK_NULL_HANDLE)
            {
                scene_view_batch.add(sc_scene, sv);
                const auto& item = scene_view_batch.items().back();
                // The swapchain provider leaves binding.layout null — set it here (阶段4 P4d).
                rt.present_target->layout = &sb.layout;
                im.renderer_->renderSingleView(
                    *item.scene, *item.view, *rt.present_target, rt, item.cross_view_index);
            }
        }

        // 5. End-of-view-frame for each touched scene
        {
            for (auto* scene : scene_view_batch.touchedScenes())
                scene->endViewFrame(stamp.slotIndex());
        }

        // 6. End frame — end CB, submit, present
        if (im.frame_driver_)
            (void)im.frame_driver_->endFrame(rt, sc_ptr);

        // GC retired offscreen images now that this frame's submit is done
        for (auto& e : im.offscreen_views_)
            e.pool->collectRetired(stamp.serial, im.frames_in_flight_);

        // Async readbacks: settle / submit / poll + send deferred replies. After
        // endFrame so a just-settled copy is ordered behind this tick's render on
        // the graphics queue (same reasoning as the synchronous handler).
        pollPendingReadbacks();

        // 7. End renderer frame + advance counters
        im.endTickFrame(rt.primary_cmd != VK_NULL_HANDLE);

        return true;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Server-side direct initialization API
    // ─────────────────────────────────────────────────────────────────────
    FeatureTypeRegisteredReply GeneralRenderServer::addFeatureFactory(const FeatureFactory& factory)
    {
        // The feature-type registry now lives in the Renderer (阶段 3), created by
        // init(). Registering a feature type therefore requires an initialized
        // server — return an invalid reply (feature_type_id 0) if called too early.
        if (!impl_->renderer_)
            return FeatureTypeRegisteredReply{};

        auto& registry = impl_->renderer_->featureTypeRegistry();

        // Register the TYPE first with NO ops (the registry dedups by factory identity, it
        // doesn't need op ids), then bind op handlers ONLY on the Registered branch — an
        // idempotent re-register must not call register_ops_fn again (each call allocates
        // fresh dispatcher slots → leak, 五-2). Mirrors handleRegisterFeatureType.
        FeatureTypeRecord rec{};
        rec.factory = factory;
        auto result = registry.add(std::move(rec));

        FeatureTypeRegisteredReply reply{};
        if (!result)
        {
            std::cerr << "[RenderServer] addFeatureFactory rejected '"
                      << (factory.name ? factory.name : "?") << "': "
                      << result.error().message() << "\n";
            return reply;   // feature_type_id = 0
        }

        reply.feature_type_id = result->type_id;
        reply.status          = static_cast<std::uint32_t>(result->status);

        FeatureTypeRecord& stored = registry.at(result->type_id);
        if (result->status == EFeatureTypeRegisterStatus::Registered && factory.register_ops_fn)
        {
            stored.op_count = factory.register_ops_fn(&impl_->dispatcher, stored.ops, 16);
            if (stored.op_count > FeatureTypeRegistry::kMaxOps)
                stored.op_count = FeatureTypeRegistry::kMaxOps;
        }
        reply.op_count = stored.op_count;
        std::copy_n(stored.ops, stored.op_count, reply.ops);
        return reply;
    }

    std::vector<FeatureTypeRegisteredReply> GeneralRenderServer::addFeatureFactories(std::span<const FeatureFactory> factories)
    {
        std::vector<FeatureTypeRegisteredReply> results;
        results.reserve(factories.size());
        for (auto& f : factories)
            results.push_back(addFeatureFactory(f));
        return results;
    }

    GeneralRenderServer::CreateSceneResult GeneralRenderServer::createScene(std::string_view name, std::span<const FeatureInitParam> features, lux::common::ETextureFormat lit_color_format)
    {
        RenderScene::Config config{};
        config.scene_name = std::string(name);
        config.pipeline.lit_color_format = lit_color_format;
        auto scene_result = impl_->renderer_->addScene(std::move(config));

        CreateSceneResult result;
        result.scene_id = scene_result.scene_id;

        auto* scene = impl_->renderer_->getScene(result.scene_id);
        for (auto& fp : features)
        {
            auto& rec = impl_->renderer_->featureTypeRegistry().at(fp.feature_type_id);
            const FeatureDescriptor& desc = rec.factory.descriptor;

            // Dependency / conflict / multiplicity enforcement is inside addFeatureImpl
            // now (三-2 unified entry): create_fn returns an invalid handle if rejected,
            // so initial features still must be listed in dependency order. Descriptor
            // visible during attach via the scope. (Guard on scene — getScene may have
            // failed; the old code would have crashed in create_fn(null,...).)
            FeatureHandle fid{};
            if (scene)
            {
                RenderScene::FeatureInstallScope install_scope(*scene, desc);
                fid = rec.factory.create_fn(scene, fp.param, fp.param_size);
            }
            result.features.push_back(fid);
        }

        return result;
    }

    Expected<ViewHandle> GeneralRenderServer::createView(const ViewInitParam& p)
    {
        auto* scene = impl_->renderer_->getScene(p.scene_id);
        if (!scene)
            return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));

        ViewCreateInfo ci{
            .initial_extent = p.extent,
            .debug_name     = p.name.data(),
        };
        ViewHandle handle = scene->addView(ci);

        // All views created via createView are offscreen
        RenderTargetLayout layout;
        {
            RenderTargetSlotDesc color{};
            color.format       = VK_FORMAT_B8G8R8A8_SRGB;
            color.usage        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            color.aspect       = VK_IMAGE_ASPECT_COLOR_BIT;
            color.final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.is_presentable = false;
            layout.slots[static_cast<size_t>(TargetSlot::SceneColor)] = color;

            RenderTargetSlotDesc depth{};
            depth.format       = VK_FORMAT_D32_SFLOAT;
            depth.usage        = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            depth.aspect       = VK_IMAGE_ASPECT_DEPTH_BIT;
            depth.final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.is_presentable = false;
            layout.slots[static_cast<size_t>(TargetSlot::SceneDepth)] = depth;
        }

        VkExtent2D vk_extent{p.extent.width, p.extent.height};
        auto pool = std::make_unique<OffscreenImagePool>(
            *impl_->res_ctx_, layout, vk_extent, impl_->frames_in_flight_
        );
        scene->compileGraphTemplate(layout);

        if (scene->getView(handle))
            impl_->offscreen_views_.push_back({
                p.scene_id, handle,
                std::move(pool), layout
            }
        );

        return handle;
    }

    Expected<void> GeneralRenderServer::bindSwapchain(
        RenderSceneId scene_id, ViewHandle view,
        const RenderTargetLayout& layout)
    {
        return bindSwapchainInternal(
            *impl_, scene_id, view, layout, /*replace_existing=*/false);
    }

    void GeneralRenderServer::unbindSwapchain()
    {
        impl_->swapchain_binding_.reset();
    }

    bool GeneralRenderServer::hasSwapchainBinding() const noexcept
    {
        return impl_->swapchain_binding_.has_value();
    }

    const RenderTargetLayout& GeneralRenderServer::swapchainLayout() const
    {
        impl_->cached_swapchain_layout_ = impl_->swapchain_provider_->layout();
        return impl_->cached_swapchain_layout_;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Server-side direct resource creation (same thread, init → tick)
    // ─────────────────────────────────────────────────────────────────────

    // (GeneralRenderServer::uploadMesh — the synchronous server-side mesh upload —
    //  was REMOVED: it had no callers after mesh data upload moved to the StandardMeshStack
    //  feature op (the production path is the async serverUploadMesh shim). The exported
    //  serverUploadMesh does the async allocate + worker submit.)

    // (GeneralRenderServer::uploadMaterial + MaterialResources::submit(rdesc::Material)
    //  retired in W5a — the builtin closure material families were removed.)

    // (GeneralRenderServer::uploadGraphMaterial removed — the graph-material submit +
    //  scene-graph invalidation is now the StandardMaterial feature's serverUploadGraphMaterial,
    //  inlined into the feature TU (MaterialOperationHandlers.cpp, Stage C). The core
    //  server no longer exposes a material-upload method.)

    Expected<RTextureHandle> GeneralRenderServer::createTexture2D(
        const lux::rdesc::Texture& texture, bool generate_mips)
    {
        auto* tex_res = impl_->render_ctx_->globalRegistry().find<TextureResources>();
        if (!tex_res)
            return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));

        auto result = tex_res->submit(texture, nullptr, VK_FORMAT_UNDEFINED, generate_mips);
        if (!result)
            return lux::cxx::unexpected(result.error());

        auto h = result.value();
        return RTextureHandle{h.index, h.gen};
    }

    // (GeneralRenderServer::createLight removed — light creation is feature-scoped:
    //  LightOperationHandlers.cpp inlines this same find<LightResources> + submit.)

    ShaderHandle GeneralRenderServer::compileShader(
        std::span<const std::byte> spirv,
        const lux::rdesc::ShaderInfo* info)
    {
        auto* shader_res = impl_->render_ctx_->globalRegistry().find<ShaderResources>();
        if (!shader_res)
            return ShaderHandle{};

        lux::rdesc::ShaderInfo default_info{};
        return shader_res->add(spirv, info ? *info : default_info);
    }

    // (GeneralRenderServer::addMeshInstance removed — the mesh-instance assembly is now
    //  the StandardMeshStack feature's serverAddMeshInstance, inlined into the feature TU
    //  (MeshStackOperationHandlers.cpp, Stage C). The core server no longer exposes a
    //  mesh-instance method or the MeshInstanceParam type.)

    Expected<void> GeneralRenderServer::flushPendingGpuTransfers()
    {
        auto& rc = *impl_->res_ctx_;
        VkDevice device = rc.logicalDevice();
        VkCommandPool cp = rc.commandPool().handle();

        // Allocate one-shot command buffer
        VkCommandBufferAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = cp;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS)
            return lux::cxx::unexpected(make_error_code(ERenderError::InternalError));

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        // Record mesh staging copies
        auto* mesh_res = impl_->render_ctx_->globalRegistry().find<MeshResources>();
        if (mesh_res)
            mesh_res->flushPendingTransfers(cmd, 0);

        // Record texture staging copies
        auto* tex_res = impl_->render_ctx_->globalRegistry().find<TextureResources>();
        if (tex_res)
        {
            tex_res->bindlessSet2D().flushUploads(cmd, 0);
            tex_res->bindlessSetCube().flushUploads(cmd, 0);
        }

        vkEndCommandBuffer(cmd);

        // Create fence
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(device, &fi, nullptr, &fence);

        // Submit
        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;
        vkQueueSubmit(rc.graphicsQueue(), 1, &submit, fence);
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

        // Retire staging buffers
        if (mesh_res)
            mesh_res->retireFrameStagingBuffers(0);
        if (tex_res)
        {
            tex_res->bindlessSet2D().retireDeferredStaging(0);
            tex_res->bindlessSetCube().retireDeferredStaging(0);
        }

        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, cp, 1, &cmd);
        return {};
    }

} // namespace lux::render
