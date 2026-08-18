#include <lux/engine/render/comm/server/RenderServerImpl.hpp>
// (InitialViewCamera.hpp retired — initial camera is a StandardViewCamera op now.)
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/render/comm/RenderTickPipeline.hpp>

// VMA — readback staging buffer uses raw vmaCreateBuffer/vmaInvalidateAllocation
// directly (previously pulled in transitively via SkinningResources.hpp, which
// moved to a feature; include what we use).
#include <vk_mem_alloc.h>

#include <mutex>

// Vulkan infrastructure
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/transfer/TransferContributor.hpp>
#include <lux/engine/render/renderer/Renderer.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

// Targets
#include <lux/engine/render/targets/OffscreenImagePool.hpp>
#include <lux/engine/render/gpu/RenderSurface.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>
#include <lux/engine/render/renderer/FrameDriver.hpp>

// Window
#include <lux/engine/window/LuxWindow.hpp>

// Resources
#include <lux/engine/render/resources/TextureResources.hpp>
// (LightResources include removed — light is feature-owned now; LightFeature
//  emplaces the per-scene LightResources, not the core server.)
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/gpu/pipeline/VertexLayoutRegistry.hpp>   // vertex-layout SSOT
#include <lux/engine/render/gpu/pipeline/VertexLayoutSpec.hpp>       // make*VertexLayout()
#include <lux/engine/render/resources/lighting/ShadowResources.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/vertex/StaticVertexPoolSet.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/descriptor/BindlessCombinedSet.hpp>
#include <lux/engine/render/resources/lifecycle/GpuTransferPipeline.hpp>
#include <lux/engine/render/gpu/lifecycle/VRAMBudgetGuard.hpp>

// Resource descriptions (rdesc)
#include <lux/engine/description/MaterialEnums.hpp>   // rdesc::EAlphaMode (graph render-state)
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Texture.hpp>

// Shader compilation
#include <lux/engine/render/gpu/ShaderObject.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/description/Shader.hpp>

// Scene / View
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/function/render/client/core/RenderViewTypes.hpp>
#include <lux/engine/render/resources/material/MaterialFamily.hpp>
// (LightDescriptor include removed — light commands are feature-scoped now.)

#include <lux/cxx/container/SparseSet.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <sstream>      // DumpRenderGraph → in-memory text capture
#include <limits>
#include <unordered_map>
#include <lux/engine/render/gpu/lifecycle/VRAMBudgetGuard.hpp>

namespace lux::render
{
    namespace
    {
        [[nodiscard]] lux::render::CapacityDomainId ownCapacityId(
            lux::render::CapacityDomainIdView id)
        {
            return lux::render::CapacityDomainId{
                id.name()};
        }

        [[nodiscard]] lux::cxx::expected<
            lux::render::CapacityCatalog,
            lux::render::CapacityCatalogError>
        makeCapacityCatalog(
            const lux::render::DeviceCapabilities& device)
        {
            using namespace lux::render;
            CapacityCatalog catalog;

            const auto instance_device_limit =
                device.buffer_device_address && device.shader_int64
                ? 0xffffffffull
                : std::min<std::uint64_t>(
                    0xffffffffull,
                    device.max_storage_buffer_range / 80u);
            if (auto result = catalog.add(CapacityDomainDescriptor{
                    .id = ownCapacityId(kActiveInstancesCapacity),
                    .device_limit = instance_device_limit,
                    .protocol_limit = 0xffffffffull,
                    .automatic_target = 65'536u,
                    .minimum = 16'384u,
                    .units_per_granule = 16'384u,
                    .bytes_per_granule = 16'384u * 280u,
                }); !result)
            {
                return lux::cxx::unexpected(result.error());
            }
            if (auto result = catalog.add(CapacityDomainDescriptor{
                    .id = ownCapacityId(kClassicMeshRecordsCapacity),
                    .device_limit = 0xffffffffull,
                    .protocol_limit = 0xffffffffull,
                    .automatic_target = 65'536u,
                    .minimum = 4'096u,
                    .units_per_granule = 4'096u,
                    .bytes_per_granule = 4'096u * 512u,
                }); !result)
            {
                return lux::cxx::unexpected(result.error());
            }
            constexpr auto kGeometryGranule = 32ull * 1024u * 1024u;
            if (auto result = catalog.add(CapacityDomainDescriptor{
                    .id = ownCapacityId(kClassicMeshGeometryBytesCapacity),
                    .device_limit = std::numeric_limits<std::uint64_t>::max(),
                    .protocol_limit = std::numeric_limits<std::uint64_t>::max(),
                    .automatic_target = 512ull * 1024u * 1024u,
                    .minimum = 96ull * 1024u * 1024u,
                    .units_per_granule = kGeometryGranule,
                    .bytes_per_granule = kGeometryGranule,
                }); !result)
            {
                return lux::cxx::unexpected(result.error());
            }
            return catalog;
        }
    } // namespace

    namespace detail
    {
        Expected<void> bindSwapchainInternal(
            GeneralRenderServer::Impl& impl,
            RenderSceneId scene_id,
            ViewHandle view,
            const RenderTargetLayout& layout,
            bool replace_existing
        );
    }

    static std::vector<const char*> toCharPtrVec(const std::vector<std::string>& strs)
    {
        std::vector<const char*> result;
        result.reserve(strs.size());
        for (auto& s : strs) result.push_back(s.c_str());
        return result;
    }

    Expected<void> GeneralRenderServer::Impl::init(ServerConfig cfg)
    {
        // Validate the fixed-size frame ring before publishing any Vulkan
        // infrastructure into Impl. An invalid configuration must leave the
        // object in its pristine, trivially destructible state.
        if (cfg.frames_in_flight < 1 || cfg.frames_in_flight > kMaxFramesInFlight)
        {
            return renderFailure<err::device::InvalidFramesInFlight>(
                cfg.frames_in_flight,
                kMaxFramesInFlight
            );
        }

        // 0. Create Vulkan infrastructure (InstanceContext ctor may throw)
        DebugCallback debug_cb = nullptr;
        if (cfg.enable_validation)
        {
            // 校验层的回调没有"调用方"可以交待 —— 走自发上报。消息文本过不了 comm
            // (实参槽只装数字):带走的是严重级别与消息的 FNV-1a 指纹,同一条消息
            // 因此能在一批内正确合并计数。
            //
            // 但一条 VUID 违规的诊断价值**全在全文**里(它指名道姓说了哪个字段
            // 违了哪条规则),指纹一个数字顶不上。全文因此另走 cfg 里的钩子:装了
            // 就原样交给应用,不装就丢弃。渲染库不替应用决定这些文字去哪 ——
            // 但也不能因为"库里不该打印"就让全文无处可去,那是把可诊断性一起删了。
            //
            // 写的是 validation_ring_ 而不是 error_sink_:本回调**在发起 Vulkan
            // 调用的那个线程上跑**,而上传走单一 transfer 线程 —— sink
            // 是按键线性合并的非原子结构,多线程写它就是数据竞争。环负责跨线程
            // 传输,渲染线程在 flushErrorEvents() 里排空折进 sink。
            debug_cb = [err_counter = cfg.validation_error_counter,
                        text_sink   = cfg.validation_message_sink,
                        ring        = &validation_ring_](const DebugCallbackInfo& info) -> bool {
                // Mesh passes deliberately use one fixed vertex-stage superset
                // interface across builtin and graph-material fragment stages.
                // Vulkan permits an FS to consume only a subset, but the
                // validation layer emits WARNING-Shader-OutputNotConsumed once
                // per resulting pipeline. There is no hazard and specializing
                // the VS for every subset would defeat the shared pipeline
                // family contract, so suppress only this exact performance
                // advisory. All VUIDs, synchronization hazards and other
                // performance warnings continue through both outlets.
                if ((info.flags & VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT)
                    && info.message != nullptr
                    && std::string_view{info.message}.find(
                           "WARNING-Shader-OutputNotConsumed"
                       ) != std::string_view::npos)
                    return false;

                if (err_counter && (info.flags & VK_DEBUG_REPORT_ERROR_BIT_EXT))
                    err_counter->fetch_add(1, std::memory_order_relaxed);

                std::uint32_t severity = 0;   // 0=info 1=warn 2=error
                if (info.flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
                {
                    severity = 2;
                }
                else if (info.flags &
                         (VK_DEBUG_REPORT_WARNING_BIT_EXT |
                          VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT))
                {
                    // Synchronization Validation reports SYNC-HAZARD
                    // diagnostics through the performance-warning bit. Do
                    // not demote those to Info: under diagnostic load the
                    // host may shed Info records first, losing the VUID and
                    // named command/resource context while the numeric
                    // fingerprint survives.
                    severity = 1;
                }

                std::uint32_t fingerprint = 2166136261u;
                for (const char* p = info.message; p != nullptr && *p != '\0'; ++p)
                {
                    fingerprint ^= static_cast<std::uint8_t>(*p);
                    fingerprint *= 16777619u;
                }

                ring->emit(severity, fingerprint);

                if (text_sink && info.message != nullptr)
                    text_sink(severity, std::string_view{info.message});
                return false;
            };
        }
        inst_ctx_ = std::make_unique<InstanceContext>(cfg.instance_extensions, std::move(debug_cb));
        dev_ctx_  = std::make_unique<DeviceContext>(*inst_ctx_);
        res_ctx_  = std::make_unique<ResourceContext>(*dev_ctx_);

        frames_in_flight_ = cfg.frames_in_flight;
        // 目标注册表要用设备资源上下文造默认池,设备就绪后立即注入。
        targets_registry_.init(*res_ctx_, frames_in_flight_);
        frame_orchestrator_ = FrameOrchestrator{cfg.frames_in_flight};
        enable_vsync_     = cfg.enable_vsync;

        if (auto r = dev_ctx_->init(cfg.prefer_discrete_gpu
                              ? EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED
                              : EPhysicalDeviceSelectionPolicy::INTEGRATED_GPU_PREFERRED); !r)
            return lux::cxx::unexpected(r.error());

        // Resolve the session feature tier = min(device-achievable, caller
        // preference). Features read dev_ctx caps()/featureLevel() at attach
        // to pick implementation variants (mobile-adaptation topic ①).
        // (原先这里往 stderr 打一行「解析出的等级 / 可达等级 / 请求等级 + local_read」。
        //  删掉了:这些现在都可查 —— GeneralRenderServer::deviceCaps() 与 comm 的
        //  QueryDeviceCaps 回执都带 feature_level 与完整 DeviceCaps。当时非打不可,
        //  是因为除了那一行之外没有任何办法知道自己跑在哪一档上。)
        dev_ctx_->resolveFeatureLevel(cfg.preferred_level);

        const auto vram = VRAMBudgetGuard(
            dev_ctx_->vmaAllocator()).snapshot();
        const auto& device_caps = dev_ctx_->caps();
        const lux::render::DeviceCapabilities capacity_caps{
            .vram_budget_bytes = vram.total_budget,
            .vram_usage_bytes = vram.total_usage,
            .max_storage_buffer_range =
                device_caps.max_storage_buffer_range,
            .buffer_device_address =
                device_caps.buffer_device_address,
            .shader_int64 = device_caps.shader_int64,
        };
        auto capacity_catalog = makeCapacityCatalog(capacity_caps);
        if (!capacity_catalog)
            return renderFailure<err::internal::Unspecified>();
        auto capacity_plan = lux::render::makeCapacityPlan(
            cfg.capacity_request,
            capacity_caps,
            *capacity_catalog);
        if (!capacity_plan)
        {
            if (cfg.capacity_shortfall_output)
                *cfg.capacity_shortfall_output = capacity_plan.error();
            return renderFailure<err::memory::CapacityExhausted>();
        }
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
            return renderFailure<err::internal::Unspecified>();
        auto& layouts = *descriptor_layouts;

        // 2. Pipeline manager (variants only use normalized constants; no compiler instance needs injecting)
        auto pipeline_mgr = std::make_unique<PipelineManager>(device_ctx, cfg.use_dynamic_rendering);

        // 3. Global resource registry
        auto global_reg = std::make_unique<ResourceRegistry>();

        // TextureResources
        {
            // Bindless capacity comes FROM THE LAYOUT, not from a second
            // derivation off the device limits.
            //
            // These numbers size a descriptor pool, and what the driver
            // charges that pool is the binding counts in the Texture SET
            // LAYOUT — so any independently-derived value is only ever
            // accidentally right. This site used to re-derive it and apply a
            // 64K ceiling the layout did not, which meant the layout declared
            // the full device budget while the pool was sized for the capped
            // one: on Adreno 830 (~16.7M UAB sampled images) that is a request
            // for 16,776,952 descriptors out of a pool of 65,792, i.e.
            // VK_ERROR_OUT_OF_POOL_MEMORY at startup. Desktop NVIDIA never
            // complained because it does not account pool capacity per-type.
            const uint32_t tex2d_max = layouts.bindless2DCount();
            const uint32_t cube_max  = layouts.bindlessCubeCount();

            BCInitInfo bc{};
            bc.resource_context      = res_ctx_.get();
            bc.descriptor_set_layout = layouts.getLayout(EDescriptorSetSlot::Texture);
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
            // Same rule as tex2d_max: the cube table's pool share is charged
            // against the layout's binding-1 count, so take it from there
            // instead of leaving InitInfo's default to agree by coincidence.
            info.cube_max_capacity  = cube_max;
            info.slices             = fif;
            info.default_sampler_ci = sampler_ci;
            info.fallback_pixel     = std::nullopt;
            // init() 的 bool 返回值此前被忽略 —— 而资源注册表没有 erase,失败 init
            // 的对象仍留在表里、指针非空,所以下游任何"非空判断"都发现不了它。
            // 这里是唯一能判定初始化成败的地方。(下游的判空已随 must<> 收敛删除:
            // 它们检测的是"注册了没有",而真正会坏的是"init 成不成功"——测错了对象。)
            auto* tex = global_reg->emplace<TextureResources>().get();
            if (!tex->init(info))
                return renderFailure<err::internal::Unspecified>();
            // 每帧维护由**安装点**登记 —— 资源自己不再继承帧接口。
            global_reg->addBeginFrameHook(
                EUploadPhase::Upload,
                [tex](const FrameStamp& s) { tex->onFrameBeginMaintenance(s); });
        }

        // LightResources is PER-SCENE and FEATURE-OWNED: LightFeature emplaces +
        // init's it in initAndAttachTo (with the SHARED light set layout). The core
        // server/scene no longer touches light — a scene without LightFeature renders
        // unlit. No global instance exists.

        // MaterialResources (5 family SSBOs + variant buckets)
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
            auto& vlr = *global_reg->emplace<VertexLayoutRegistry>();
            vlr.registerLayout(makeDefaultVertexLayout());      // id 0
            vlr.registerLayout(makeLeanSkinnedOutputLayout());  // id 1
        }

        // ShadowResources is now PER-SCENE (Plan A): each scene's
        // ShadowMapFeature lazily emplaces a ShadowResources into the scene
        // registry. No global atlas exists anymore.

        // ShaderResources — emplaced AND init'ed here, like TextureResources above.
        // It used to be published bare and init'ed after RenderContext, on the
        // stated grounds that init "needs VkDevice". It does — but the device has
        // been up since res_ctx_->init() far above; the old code just asked
        // RenderContext for it (RenderContext::device() returns exactly this same
        // res_ctx.deviceContext().logicalDevice()). So the deferral was an
        // accident of where it looked, not a real ordering constraint — and it
        // was the last GPU resource in the module that could not be initialized
        // at publish time.
        {
            auto& shaders = *global_reg->emplace<ShaderResources>();
            shaders.init(ShaderResources::InitInfo{
                .device = device_ctx.logicalDevice(),
                .sparse_instance_pages =
                    device_caps.buffer_device_address &&
                    device_caps.shader_int64,
            });
        }

        // 5. Build RenderContext
        RenderContext::CreateInfo ci{};
        ci.pipeline_mgr         = std::move(pipeline_mgr);
        ci.descriptor_layouts   = std::move(descriptor_layouts);
        ci.global_resources     = std::move(global_reg);
        ci.frames_in_flight     = fif;
        ci.capacity_plan        = *capacity_plan;

        auto render_context = RenderContext::create(*res_ctx_, std::move(ci));
        if (!render_context)
            return lux::cxx::unexpected<RenderError>(render_context.error());
        render_ctx_ = std::move(*render_context);
        // 自发上报的去处。汇集器随 Impl 存活,覆盖 render_ctx_ 的生命周期。
        render_ctx_->setErrorSink(&error_sink_);
        render_ctx_->globalTransferScheduler().contributors().add(
            makeTransferContributorWithPost(
                &render_ctx_->globalRegistry().must<TextureResources>(),
                /*priority=*/10
            )
        );

        // 6. Build Renderer
        renderer_ = std::make_unique<Renderer>(render_ctx_);

        // (7. ShaderResources 的 init 已移到它的 emplace 处 —— 见上面的说明。)

        // 7b. Inject centralized DeferredDestroyQueue into global GPU resources.
        //     Resources were created before RenderContext, so we do a late bind.
        {
            auto* q = &render_ctx_->deferredDestroyQueue();
            auto& reg = render_ctx_->globalRegistry();
            // LightResources is per-scene + feature-owned now; its deferred queue is bound in LightFeature.
            // MeshResources + MaterialResources are built lazily (ensureGlobalMesh/
            // MaterialResources) which bind their own deferred queue — nothing to
            // late-bind here.
            reg.must<TextureResources>().setDeferredQueue(q);
        }

        // 8. Single-owner GPU transfer pipeline
        {
            GpuTransferPipeline::Config ucfg{};
            ucfg.device_ctx = dev_ctx_.get();
            ucfg.notify_work_state = server_;
            ucfg.notify_work = +[](void* state) noexcept
            {
                static_cast<GeneralRenderServer*>(state)
                    ->channelSync().notifyRequestStateChanged();
            };
            ucfg.lifecycle_state = this;
            ucfg.lifecycle = +[](
                void* state,
                std::uint32_t request_id,
                TransferCompletion::Kind kind,
                std::uint32_t resource_index,
                std::uint32_t resource_gen,
                EUploadLifecycleState lifecycle_state) noexcept
            {
                static_cast<Impl*>(state)->transitionUpload(
                    request_id,
                    kind,
                    resource_index,
                    resource_gen,
                    lifecycle_state
                );
            };
            auto pipeline = GpuTransferPipeline::create(ucfg);
            if (!pipeline)
                return lux::cxx::unexpected<RenderError>(pipeline.error());
            transfer_pipeline_ = std::move(*pipeline);
        }

        // 9. FrameDriver — 帧级设施(per-FIF fence/主 CB/GPU 完成水位),
        // 仅依赖 ResourceContext 与 fif,与窗口/surface 零依赖。曾误放在
        // attachToWindow 里,导致无窗进程只能空转帧生命周期不能录制;
        // 前移后离屏渲染 + readback 无窗即可用(RenderTarget 一等化的产物)。
        auto frame_driver = FrameDriver::create(*res_ctx_, frames_in_flight_);
        if (!frame_driver)
            return lux::cxx::unexpected<RenderError>(frame_driver.error());
        frame_driver_ = std::move(*frame_driver);

        return {};
    }

    GeneralRenderServer::Impl::~Impl()
    {
        if (!dev_ctx_) return;  // init() was never called
        bool device_lost_during_teardown = false;

        // Close admission and join the single transfer owner first. Recorded
        // RECORD_ONLY batches are then submitted by this render thread before
        // the device-idle boundary; no queue mutex is involved.
        if (transfer_pipeline_)
        {
            transfer_pipeline_->shutdown();

            // Drain the sole transfer-result SPSC. drainResults also submits
            // RECORD_ONLY batches on the graphics queue and returns their
            // completion ownership to this thread.
            uint32_t n;
            do {
                n = transfer_pipeline_->drainResults(
                    completion_buf_, kMaxDrainBatch);
                for (uint32_t i = 0; i < n; ++i)
                    pending_completions_.push_back(completion_buf_[i]);
            } while (n > 0);

            const auto transfer_idle =
                dev_ctx_->logicalDevice().waitIdle();
            device_lost_during_teardown =
                transfer_idle == VK_ERROR_DEVICE_LOST;
            if (transfer_idle != VK_SUCCESS &&
                transfer_idle != VK_ERROR_DEVICE_LOST)
                renderFatal("RenderServer transfer drain wait-idle failed");

            for (auto& pending : pending_graphics_finalizes_)
                if (pending.command_buffer != VK_NULL_HANDLE)
                    vkFreeCommandBuffers(
                        dev_ctx_->logicalDevice().handle(),
                        res_ctx_->commandPool(),
                        1,
                        &pending.command_buffer);
            pending_graphics_finalizes_.clear();
            graphics_finalize_reply_batch_.clear();
            graphics_finalize_staging_batch_.clear();
            graphics_finalize_slot_batch_.clear();

            for (auto& c : pending_completions_)
                destroyUnfinalizedCompletion(c);
            pending_completions_.clear();
            transfer_pipeline_.reset();
        }

        const auto teardown_idle = dev_ctx_->logicalDevice().waitIdle();
        device_lost_during_teardown = device_lost_during_teardown ||
            teardown_idle == VK_ERROR_DEVICE_LOST;
        if (teardown_idle != VK_SUCCESS &&
            teardown_idle != VK_ERROR_DEVICE_LOST)
            renderFatal("RenderServer failed to wait for device idle during teardown");

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

        // Free deferred staging buffers + deferred offscreen pools (device is
        // idle at shutdown — tags no longer matter).
        async_deferred_staging_.clear();

        // PresentContext has a reportable close boundary. Destructors only
        // validate that boundary; teardown must never hide queue-wait failure.
        for (auto& release : pending_surface_releases_)
            if (release.ctx)
            {
                auto closed = release.ctx->close();
                if (!closed)
                {
                    if (device_lost_during_teardown)
                        release.ctx->acknowledgeDeviceLoss();
                    else
                        renderFatal(
                            "pending PresentContext close failed during shutdown");
                }
            }
        for (auto& target : targets_registry_.all().values())
            if (target.present)
            {
                auto closed = target.present->close();
                if (!closed)
                {
                    if (device_lost_during_teardown)
                        target.present->acknowledgeDeviceLoss();
                    else
                        renderFatal(
                            "target PresentContext close failed during shutdown");
                }
            }

        // Destroy target pools + Surface presentation state before device teardown.
        // Member RAII enforces swapchain → semaphores → surface.
        pending_surface_releases_.clear();
        targets_registry_.shutdown();

        // Destroy FrameDriver (waits idle + frees sync objects)
        frame_driver_.reset();
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

    /// 装配层(L6)取服务端 Vulkan 栈的窄入口 —— 与 lookupScene 同一条约定。
    ///
    /// 为什么要有这一组:装配 TU 需要的只是**几个具体的东西**(渲染上下文、
    /// 上传池、"对所有场景做一件事"),而不是整个 Impl。让它们各自
    /// `static_cast<GeneralRenderServer::Impl*>(user_state)` 就意味着每个装配 TU
    /// 都得 include 服务端的 Impl 头,于是 Impl 的每个字段都成了装配层的接口。
    /// 24 个装配 TU 里有 22 个遵守这条约定,只有 meshstack/material 两个破例
    /// —— 破例的归队,而不是把约定改掉。
    RenderContext* lookupRenderContext(void* user_state)
    {
        auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
        return im.render_ctx_.get();
    }

    /// 共享异步上传池(网格 + 纹理共用)。
    GpuTransferPipeline* lookupTransferPipeline(void* user_state)
    {
        auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
        return im.transfer_pipeline_.get();
    }

    /// 对本服务器的每个场景做一件事。材质上传改变了全局材质栈的形状,所有
    /// 场景的渲染图都得重编 —— 这是唯一需要"遍历全部场景"的装配路径。
    void forEachSceneOnServer(void* user_state, void (*fn)(RenderScene&))
    {
        auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
        if (im.renderer_)
            im.renderer_->forEachScene([fn](RenderScene& s) { fn(s); });
    }

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

        // ── Scene lifecycle ──────────────────────────────────────────────────

        /// Offscreen 目标缺省 layout(格式统一 B8G8R8A8_SRGB + D32;format_key
        /// 定制随编译 key 拆解开放)。sampled=false:可 readback(TRANSFER_SRC,
        /// final=COLOR_ATTACHMENT);sampled=true:可被采样(SAMPLED,
        /// final=SHADER_READ_ONLY——ImGui 面板/后续 pass 消费)。
        RenderTargetLayout makeOffscreenTargetLayout(bool sampled)
        {
            RenderTargetLayout layout;
            RenderTargetSlotDesc color{};
            color.format       = lux::common::ETextureFormat::BGRA8_SRGB;
            color.aspect       = ERenderAspect::COLOR;
            color.is_presentable = false;
            if (sampled)
            {
                // TRANSFER_SRC 保留:SAMPLED 目标同样可 readback(缩略图
                // 服务对预览视图的异步回读依赖此位)。
                color.usage = ERenderImageUsage::COLOR_ATTACHMENT |
                              ERenderImageUsage::SAMPLED |
                              ERenderImageUsage::TRANSFER_SOURCE;
                color.final_state = ERenderResourceState::SHADER_READ;
            }
            else
            {
                color.usage = ERenderImageUsage::COLOR_ATTACHMENT |
                              ERenderImageUsage::TRANSFER_SOURCE;
                color.final_state = ERenderResourceState::COLOR_ATTACHMENT;
            }
            layout.slots[static_cast<size_t>(TargetSlot::SCENE_COLOR)] = color;

            RenderTargetSlotDesc depth{};
            depth.format = lux::common::ETextureFormat::D32_SFLOAT;
            depth.usage = ERenderImageUsage::DEPTH_STENCIL_ATTACHMENT;
            depth.aspect = ERenderAspect::DEPTH;
            depth.final_state = ERenderResourceState::DEPTH_STENCIL_ATTACHMENT;
            depth.is_presentable = false;
            layout.slots[static_cast<size_t>(TargetSlot::SCENE_DEPTH)] = depth;
            return layout;
        }

        void handleCreateScene(Ctx& ctx, const CreateScenePayload& p)
        {
            auto& im = impl(ctx);
            RenderScene::Config config{};
            config.scene_name = p.name;
            config.pipeline.lit_color_format = p.lit_color_format;
            config.coordinate_page_size = p.coordinate_page_size;
            for (std::size_t i = 0; i < 3; ++i)
                config.scene_origin_page[i] = p.scene_origin_page[i];

            auto result = im.renderer_->addScene(std::move(config));

            SceneCreatedReply reply{};
            reply.scene_id = result.scene_id;
            replyToCurrent<CreateScenePayload>(ctx, reply);
        }

        void handleDestroyScene(Ctx& ctx, const DestroyScenePayload& p)
        {
            auto& im = impl(ctx);
            if (!im.renderer_->getScene(p.scene_id)) return;

            // NO full-device vkDeviceWaitIdle here: it stalled every OTHER scene
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

            // Scene 销毁级联:摘除全部引用该 scene 的层(层引用不拥有——
            // 设计不变量③);Offscreen target 因此空链的,池按 fence 水位
            // 延迟释放后整个 target 消亡;Surface target 只摘层不消亡。
            {
                const auto keys = im.targets_registry_.all().keys();   // 拷贝:循环内 erase
                for (const auto key : keys)
                {
                    auto* t = im.targets_registry_.tryGet(key);
                    if (!t) continue;
                    bool removed = false;
                    for (size_t i = t->layers.size(); i-- > 0;)
                        if (t->layers[i].scene_id == p.scene_id)
                        {
                            t->layers.erase(t->layers.begin() + i);
                            removed = true;
                        }
                    if (removed && t->layers.empty() &&
                        t->kind == GeneralRenderServer::Impl::RenderTargetEntry::EKind::Offscreen)
                    {
                        im.retireTargetPool(*t, im.current_stamp_.serial);
                        im.targets_registry_.erase(key);
                    }
                }
            }
        }

        void handleRebaseSceneOrigin(
            Ctx& ctx,
            const RebaseSceneOriginPayload& payload)
        {
            auto& im = impl(ctx);
            auto* scene = im.renderer_->getScene(payload.scene_id);
            if (scene == nullptr)
                return;
            auto rebased = scene->rebaseSceneOrigin(
                payload.scene_origin_page);
            if (!rebased)
            {
                im.error_sink_.emit(
                    rebased.error(),
                    payload.scene_id.index,
                    im.current_stamp_.serial);
            }
        }

        void handleAddView(Ctx& ctx, const AddViewPayload& p)
        {
            auto& im = impl(ctx);
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (sc == nullptr)
            {
                replyToCurrent<AddViewPayload>(ctx,
                    ViewCreatedReply{{}, renderError<err::scene::NotFound>(p.scene_id.index)});
                return;
            }

            const ViewCreateInfo ci{
                .initial_extent = p.extent,
                .debug_name     = p.name,
            };
            const ViewHandle handle = sc->addView(ci);

            if (!handle.isValid())
            {
                // addView 只在视图槽位耗尽时交回无效句柄(见 SceneViewSet)。
                replyToCurrent<AddViewPayload>(ctx,
                    ViewCreatedReply{{}, renderError<err::memory::CapacityExhausted>()});
                return;
            }

            // 视图不再隐带渲染目标:客户端随后 createOffscreenRenderTarget +
            // setLayer 显式接线(或 bindSwapchain 上屏)。无 target 引用的
            // 视图不渲染——tick 只遍历 target 的合成链。

            // (Initial camera removed from AddView — the View type no longer bakes
            // in 3D-specific concepts: the client sends a StandardViewCamera op for
            // this view after addView. AddView is neutral.)

            replyToCurrent<AddViewPayload>(ctx, ViewCreatedReply{handle});
        }

        void handleRemoveView(Ctx& ctx, const RemoveViewPayload& p)
        {
            auto& im = impl(ctx);
            GenericOkReply reply{};
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (sc == nullptr)
            {
                reply = GenericOkReply{1,
                    renderError<err::scene::NotFound>(p.scene_id.index)};
            }
            else if (!sc->removeView(p.view))
            {
                reply = GenericOkReply{2,
                    renderError<err::scene::ViewNotRemovable>(p.view.index)};
            }

            // View 销毁级联摘层(不变量③)。空链 Offscreen target 的池改走
            // fence 水位延迟释放——拉平此前 base 路径"立即析构"的
            // in-flight 风险(设计消亡清单:立即析构 → 统一 fence 门控)。
            // 摘除被拒也照跑:合成链上不该再有这个 view 的层(幂等清扫)。
            const auto keys = im.targets_registry_.all().keys();
            for (const auto key : keys)
                im.detachLayerAndReapIfEmpty(key, p.scene_id, p.view,
                                             im.current_stamp_.serial);

            // 回执在级联摘层之后 —— code==0 代表「视图摘了,层也摘了」。
            replyToCurrent<RemoveViewPayload>(ctx, reply);
        }

        // (handleResizeView 已消亡:视图不再自持尺寸账本——current_extent
        //  由渲染期从 target binding 派生;改尺寸走 ResizeTarget 直达图像池,
        //  View 瘦身。)

        //(handleSetSceneTime 已随 SetSceneTime op 退役 —— 客户端从来没有对应
        // 方法,这个 handler 恒不可达。注意 RenderScene::setSceneTime 本身还在,
        // 它现在零调用方,是否也该删单独判。)

        // ── RenderTarget 一等化命令处理器(设计 §1)──────────────────

        // Clamp a requested target extent to the device's framebuffer / image
        // limits(原 UI 层 clampViewExtent 的职责,随命令面下沉到 base)。
        VkExtent2D clampTargetExtent(GeneralRenderServer::Impl& im, common::Size2D extent)
        {
            const auto& limits = im.res_ctx_->deviceContext()
                                     .physicalDevice().properties().properties.limits;
            const uint32_t max_w = std::min(limits.maxFramebufferWidth,  limits.maxImageDimension2D);
            const uint32_t max_h = std::min(limits.maxFramebufferHeight, limits.maxImageDimension2D);
            return {std::clamp(extent.width,  1u, max_w),
                    std::clamp(extent.height, 1u, max_h)};
        }

        void handleCreateOffscreenTarget(Ctx& ctx, const CreateOffscreenTargetPayload& p)
        {
            auto& im = impl(ctx);
            TargetReadyReply reply{};
            if (p.extent.width == 0 || p.extent.height == 0)
            {
                reply.status = 1;
                replyToCurrent<CreateOffscreenTargetPayload>(ctx, reply);
                return;
            }

            const RenderTargetLayout layout =
                makeOffscreenTargetLayout((p.flags & kTargetFlagSampled) != 0);
            const VkExtent2D vk_extent = clampTargetExtent(im, p.extent);

            GeneralRenderServer::Impl::RenderTargetEntry entry{};
            entry.kind   = GeneralRenderServer::Impl::RenderTargetEntry::EKind::Offscreen;
            entry.flags  = p.flags;
            entry.layout = layout;
            // 经扩展点创建:UI 层对 SAMPLED 目标返回 UIOffscreenImagePool
            //(ImGui 描述符),其余走基类池。
            entry.pool   = im.makeTargetPool(layout, vk_extent, p.flags);
            reply.target = im.targets_registry_.insert(std::move(entry));
            replyToCurrent<CreateOffscreenTargetPayload>(ctx, reply);
        }

        void handleCreateSurfaceTarget(Ctx& ctx, const CreateSurfaceTargetPayload& p)
        {
            auto& im = impl(ctx);
            TargetReadyReply reply{};
            RenderSurface surface;
            if (p.native_window_handle == 0 ||
                !surface.initFromNative(p.native_window_handle,
                                        VkExtent2D{p.extent.width, p.extent.height},
                                        im.inst_ctx_->instance()))
            {
                reply.status = 1;
                replyToCurrent<CreateSurfaceTargetPayload>(ctx, reply);
                return;
            }
            auto r = im.createSurfaceTargetInternal(
                std::move(surface), VkExtent2D{p.extent.width, p.extent.height});
            if (!r)
            {
                reply.status = 2;
                replyToCurrent<CreateSurfaceTargetPayload>(ctx, reply);
                return;
            }
            reply.target = *r;
            replyToCurrent<CreateSurfaceTargetPayload>(ctx, reply);
        }

        void handleDestroyTarget(Ctx& ctx, const DestroyTargetPayload& p)
        {
            auto& im = impl(ctx);
            auto* t = im.targets_registry_.tryGet(p.target);
            if (!t)
            {
                // 幂等:不存在也回执,宿主的关窗等待不至于挂死。
                replyToCurrent<DestroyTargetPayload>(ctx, TargetReleasedReply{p.target, 1u});
                return;
            }

            if (t->kind == GeneralRenderServer::Impl::RenderTargetEntry::EKind::Surface)
            {
                // 两阶段销毁前半程:立即停止呈现(surface_target_ 清空 →
                // 后续 tick 走离屏路径,不再 acquire),呈现机件随在途账本
                // 移出 entry,等 fence 水位证明在飞帧全部走完(见 tick GC)。
                if (p.target == im.targets_registry_.surfaceTargetId())
                    im.targets_registry_.setSurfaceTarget({});
                t->layers.clear();
                GeneralRenderServer::Impl::PendingSurfaceRelease rel{};
                rel.target        = p.target;
                // 退休阈值 = 受理时已提交的最后一帧,不是 current serial:
                // target 此刻已摘除,之后提交的帧不再引用它;而 current serial
                // 属于本 tick——本 tick 可能因 targets 已空而不提交(关窗正是
                // 最后一个 target),该 serial 永远无 fence 佐证,水位永不越过。
                rel.retire_serial = im.frame_driver_
                    ? im.frame_driver_->lastSubmittedSerial() : 0;
                rel.request_id    = ctx.currentRequestId();
                rel.ctx           = std::move(t->present);
                im.targets_registry_.erase(p.target);
                im.pending_surface_releases_.push_back(std::move(rel));
                return;   // 回执延迟——TargetReleased 由 GC 步进送出
            }

            // Offscreen:池经统一退休入口(UI 池路由到 UI 侧退休列表,
            // 其余按 fence 水位延迟拆);受理即回执,客户端无需等待。
            im.retireTargetPool(*t, im.current_stamp_.serial);
            im.targets_registry_.erase(p.target);
            replyToCurrent<DestroyTargetPayload>(ctx, TargetReleasedReply{p.target, 0u});
        }

        void handleSetLayer(Ctx& ctx, const SetLayerPayload& p)
        {
            auto& im = impl(ctx);
            auto* t = im.targets_registry_.tryGet(p.target);
            auto* sc = im.renderer_->getScene(p.scene_id);
            if (!t || !sc || !sc->getView(p.view))
                return;
            // 层引用 view,不拥有;order 位存在即替换,越界即追加到尾。
            sc->compileGraphTemplate(t->layout);
            const auto layer =
                GeneralRenderServer::Impl::RenderTargetEntry::CompositeLayer::sceneView(
                    p.scene_id, p.view);
            if (p.order < t->layers.size())
                t->layers[p.order] = layer;
            else
                t->layers.push_back(layer);
        }

        void handleRemoveLayer(Ctx& ctx, const RemoveLayerPayload& p)
        {
            auto& im = impl(ctx);
            auto* t = im.targets_registry_.tryGet(p.target);
            if (!t || p.order >= t->layers.size())
                return;
            t->layers.erase(t->layers.begin() + p.order);
        }

        void handleResizeTarget(Ctx& ctx, const ResizeTargetPayload& p)
        {
            auto& im = impl(ctx);
            auto* t = im.targets_registry_.tryGet(p.target);
            if (!t || !t->pool)
                return;
            t->pool->resize(clampTargetExtent(im, p.new_extent));   // 旧图进 retire,fence 水位 GC
        }

        void handleBindSwapchain(Ctx& ctx, const BindSwapchainPayload& p)
        {
            auto& im = impl(ctx);
            auto* scp = im.swapchainProvider();
            const auto layout = scp ? scp->layout()
                                  : RenderTargetLayout{};
            (void)detail::bindSwapchainInternal(
                im,
                p.scene_id,
                p.view,
                layout,
                true
            );
        }

        // (handleRequestSwapchainScene 已消亡:命令面零调用方;上屏路径是
        //  BindSwapchain / 服务端直呼 setSwapchainScene,的
        //  createSurfaceRenderTarget 命令面接棒。)

        // (旧 scene-wide handlePick 已随 Pick op 退役。World Instance
        // picking 是 Render Cluster feature 的 scissored ID/depth GPU pass，
        // 不再占用 core command protocol。)

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
            if (auto* t = im.targets_registry_.tryGet(j.target);
                t && t->kind == GeneralRenderServer::Impl::RenderTargetEntry::EKind::Offscreen)
                pool = t->pool.get();
            if (!pool) return 1;

            const RenderTargetLayout rt_layout = pool->layout();
            const TargetSlot slot = j.slot;                  // which output semantic to read
            if (!rt_layout.hasSlot(slot)) return 2;

            const RenderTargetSlotDesc& slot_desc = rt_layout.slot(slot);
            const VkFormat      fmt = toVkFormat(slot_desc.format);
            const VkImageLayout from_layout =
                toVkImageLayout(slot_desc.final_state);
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
            VkResult submit_result{VK_ERROR_UNKNOWN};
            {
                const std::scoped_lock queue_lock(
                    im.dev_ctx_->graphicsQueueMutex());
                submit_result = vkQueueSubmit(
                    im.dev_ctx_->graphicsQueue(),
                    1,
                    &si,
                    j.fence
                );
            }
            if (submit_result != VK_SUCCESS)
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
                                ReadbackTargetReply& reply)
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
        // request's frame. Contract: the target must already be rendered + not
        // rendered concurrently (use between ticks / before recording a frame).
        void handleReadbackTarget(Ctx& ctx, const ReadbackTargetPayload& p)
        {
            auto& im = impl(ctx);
            GeneralRenderServer::Impl::PendingReadback j{};
            j.target       = p.target;
            j.dst_ptr      = p.dst_ptr;
            j.dst_capacity = p.dst_capacity;
            j.slot         = static_cast<TargetSlot>(p.slot);

            ReadbackTargetReply reply{};
            const uint32_t st = submitReadbackCopy(im, j);
            if (st != 0) { reply.status = st; replyToCurrent<ReadbackTargetPayload>(ctx, reply); return; }

            // Finite timeout: a GPU stall must never wedge the server thread.
            constexpr uint64_t kReadbackFenceTimeoutNs = 5'000'000'000ull; // 5 s
            const VkResult wres = vkWaitForFences(im.dev_ctx_->logicalDevice(),
                                                  1, &j.fence, VK_TRUE, kReadbackFenceTimeoutNs);
            if (wres != VK_SUCCESS)  // VK_TIMEOUT or device-lost: do not hang
            {
                freeReadbackGpu(im, j);
                reply.status = 9; replyToCurrent<ReadbackTargetPayload>(ctx, reply); return;
            }

            finishReadbackCopy(im, j, reply);
            replyToCurrent<ReadbackTargetPayload>(ctx, reply);
        }

        // Asynchronous readback: enqueue a deferred job and return WITHOUT a
        // reply. pollPendingReadbacks() settles `settle_frames` ticks, submits
        // the copy, polls the fence, and sends the deferred reply by request_id.
        // The client never blocks the calling (UI) thread.
        void handleReadbackTargetAsync(Ctx& ctx, const ReadbackTargetAsyncPayload& p)
        {
            auto& im = impl(ctx);
            GeneralRenderServer::Impl::PendingReadback j{};
            j.target       = p.target;
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

        // (handleViewFrameUpdate removed — the View type no longer bakes in
        // 3D-specific concepts: the per-view camera update is a
        // StandardViewCamera feature-scoped op now, handled in ViewCameraOperationHandlers.cpp.
        // The core dispatcher no longer registers it.)

    } // anonymous namespace

    void GeneralRenderServer::pollPendingReadbacks()
    {
        auto& im = *impl_;
        advancePendingReadbacks(im);

        const bool any_done = std::ranges::any_of(
            im.pending_readbacks_,
            [](const Impl::PendingReadback& pending)
            {
                return pending.done;
            });
        if (!any_done || control_server_->hasPendingReplyPublication())
            return;

        auto& responses = control_server_->endpoint().responses;
        auto* slot = responses.tryBeginWrite();
        if (!slot)
            return;

        FrameReplyBuilder<64> builder(*slot);
        builder.begin();
        for (auto& pending : im.pending_readbacks_)
        {
            if (pending.done)
            {
                builder.push<ReadbackTargetReply>(
                    type_ids::ReplyReadbackTarget,
                    pending.reply,
                    0,
                    pending.request_id);
            }
        }

        if (!responses.publishWrite())
            return;
        channelSync().notifyReplyProduced();
        std::erase_if(
            im.pending_readbacks_,
            [](const Impl::PendingReadback& pending)
            {
                return pending.done;
            });
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Registration
    // ─────────────────────────────────────────────────────────────────────

    // Defined in RenderServerHandlers.cpp — registers the texture/shader + feature-lifecycle
    // protocol handlers (split out to keep this TU to the server's lifecycle/frame loop).
    void registerResourceAndFeatureHandlers(GeneralRenderServer::Dispatcher& d);

    void registerServerHandlers(GeneralRenderServer::Dispatcher& d)
    {
        // ── CommandOp: Scene lifecycle ──
        d.registerUnary<CreateScenePayload,                 &handleCreateScene>      (opcodes::CommandOp, type_ids::CreateScene,       "CreateScene");
        d.registerUnary<DestroyScenePayload,                &handleDestroyScene>     (opcodes::CommandOp, type_ids::DestroyScene,      "DestroyScene");
        d.registerUnary<RebaseSceneOriginPayload,           &handleRebaseSceneOrigin>(opcodes::CommandOp, type_ids::RebaseSceneOrigin, "RebaseSceneOrigin");
        d.registerUnary<AddViewPayload,                     &handleAddView>          (opcodes::CommandOp, type_ids::AddView,           "AddView");
        d.registerUnary<RemoveViewPayload,                  &handleRemoveView>       (opcodes::CommandOp, type_ids::RemoveView,        "RemoveView");
        d.registerUnary<ReadbackTargetPayload,              &handleReadbackTarget>     (opcodes::CommandOp, type_ids::ReadbackTarget,      "ReadbackTarget");
        d.registerUnary<ReadbackTargetAsyncPayload,         &handleReadbackTargetAsync>(opcodes::CommandOp, type_ids::ReadbackTargetAsync, "ReadbackTargetAsync");
        // Mesh-instance ops (Add/Remove/Make|Hide/UpdateFlags/RenderState/UserMeta)
        // are registered dynamically by the StandardMeshStack feature's
        // register_ops_fn — no longer static core handlers.
        d.registerUnary<SetActiveScenePayload,              &handleSetActiveScene>   (opcodes::CommandOp, type_ids::SetActiveScene,    "SetActiveScene");
        // ── RenderTarget 一等化命令面──
        d.registerUnary<CreateOffscreenTargetPayload,       &handleCreateOffscreenTarget>(opcodes::CommandOp, type_ids::CreateOffscreenTarget, "CreateOffscreenTarget");
        d.registerUnary<CreateSurfaceTargetPayload,         &handleCreateSurfaceTarget>  (opcodes::CommandOp, type_ids::CreateSurfaceTarget,   "CreateSurfaceTarget");
        d.registerUnary<DestroyTargetPayload,               &handleDestroyTarget>    (opcodes::CommandOp, type_ids::DestroyTarget,     "DestroyTarget");
        d.registerUnary<SetLayerPayload,                    &handleSetLayer>         (opcodes::CommandOp, type_ids::SetLayer,          "SetLayer");
        d.registerUnary<RemoveLayerPayload,                 &handleRemoveLayer>      (opcodes::CommandOp, type_ids::RemoveLayer,       "RemoveLayer");
        d.registerUnary<ResizeTargetPayload,                &handleResizeTarget>     (opcodes::CommandOp, type_ids::ResizeTarget,      "ResizeTarget");
        d.registerUnary<BindSwapchainPayload,               &handleBindSwapchain>    (opcodes::CommandOp, type_ids::BindSwapchain,     "BindSwapchain");
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


} // namespace lux::render
