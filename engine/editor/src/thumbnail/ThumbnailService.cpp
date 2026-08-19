/**
 * @file ThumbnailService.cpp
 *
 * 缩略图服务 = **第二个 SceneRuntime**(装配归属 ADR 工作线三批 1)。
 *
 * 旧形态(925 行 job 状态机)手工对着 RenderFrameSession 编排每一步:uploadMesh /
 * compileShader / uploadGraphMaterial → addMeshInstance → makeVisible +
 * updateView → readback,并自己记账 uploaded_meshes / uploaded_textures /
 * objects 到 cleanupJobResources 逐个销毁 —— 那是把资源解析器 + 网格实例子系统
 * 的职责在编辑器里重写了一遍,而且两份实现已经漂开(空图重试、反射自愈都只有
 * 一边有)。
 *
 * 新形态:job = **世界里的一组实体**。
 *
 *   Spec(资产 id + bounds)
 *     → 生成 job 实体(MeshComponent{mesh_id, material_id} + Transform3D)
 *       + frameBounds 数学写进相机实体(Transform3D 位姿 + Camera3D fov/near/far)
 *     → WaitReady:轮询 MeshInstanceReadyComponent(批 0 的观察点:实例建成且
 *       对当前 view 代次可见)
 *     → readbackTargetAsync(沉淀帧数与旧实现相同)
 *     → Encode(CPU 池) → createTexture2D(显示纹理) → 销毁 job 实体
 *
 * 上传/引用计数/换源/离场全部由驻留胶水 + MeshInstanceSubsystem 完成;
 * 销毁 job 实体后,胶水松票 → AssetManager 归零广播 → 驻留编排
 * 收 GPU 副本。手工记账整体消亡;zombie 机制缩到只剩「readback dst 缓冲与显示
 * 纹理的在途回复」。
 */
#include <lux/engine/editor/thumbnail/ThumbnailService.hpp>
#include <lux/engine/runtime/frame/MainCloseDriver.hpp>
#include <lux/engine/editor/thumbnail/ImageCodec.hpp>
#include "thumbnail/PreviewWorldCommon.hpp"               // 预览世界共用装配件(批 2 提取)
#include <lux/engine/editor/app/LuxEditor.hpp>            // EditorRenderInfra
#include <lux/engine/resource/asset/BuiltinAssetIds.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/ui/ImGuiLuxWidgets.hpp>   // encodeTextureHandleSentinel
#include <lux/engine/log/Log.hpp>

#include <lux/engine/runtime/scene/SceneRuntime.hpp>
#include <lux/engine/runtime/render/scene/RenderSceneIntegration.hpp>
#include <lux/engine/runtime/render/scene/StandardFeaturePlan.hpp>   // previewProfile

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/render/components/3d/MeshComponent.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/render/components/MeshInstanceReadyComponent.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>

#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/common/Size2D.hpp>

#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/MainThreadStateCache.hpp>

#include <stdexec/execution.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace lux::editor
{
    // The cache's stored value — the drawable result of a finished thumbnail.
    // The requested type rides on the queue; the cache stores committed state.
    struct ThumbValue
    {
        ThumbnailSet                set;
        lux::render::RTextureHandle gpu_tex{};
        ImTextureID                 im_id{0};
    };

    // .cpp-private wrapper keeps the concrete cache type out of the public header.
    struct ThumbnailService::CacheImpl
    {
        lux::exec::MainThreadStateCache<
            lux::asset::asset_id_t,
            ThumbValue
        > cache;
    };

    // C5b: the Encode stage's CPU work (swizzle + multi-size PNG encode + decode) is
    // offloaded to the executor's CPU pool. The spawned op is SELF-CONTAINED — it owns
    // its input pixels + this shared EncodeOut, never the Job — so an abort/shutdown can
    // simply orphan it (it writes into the still-alive EncodeOut and exits). `done` is the
    // release/acquire handshake: the pool thread writes the result THEN done.store(release);
    // WaitEncode (main thread) reads done.load(acquire) THEN the result.
    struct EncodeOut
    {
        ThumbnailSet           set;
        std::vector<std::byte> display_px;
        std::uint32_t          display_w{0};
        std::uint32_t          display_h{0};
        bool                   failed{false};
        std::atomic<bool>      done{false};
    };

    // ── The private preview SceneRuntime ─────────────────────────────────────
    // This private catalogue owns only the preview descriptor. The empty
    // mapper satisfies SceneRuntime::tick; preview worlds have no input.
    struct ThumbnailService::RuntimeHost
    {
        lux::runtime::SceneContributionCatalog      contributions;
        lux::input::ActionMapper                    mapper;
        std::unique_ptr<lux::runtime::SceneRuntime> runtime;
        lux::render::RenderTargetLease              target{};
        lux::meta::entity_id                        camera{entt::null};
        lux::meta::entity_id                        key_light{entt::null};
        lux::asset::asset_id_t                      sphere_mesh_id{};
        lux::asset::asset_id_t                      preview_grey_id{};
        std::uint32_t                               render_size{256};
    };

    // ── In-flight job state (the async state machine) ────────────────────────
    struct ThumbnailService::Job
    {
        enum class Stage
        {
            Spec,        ///< build the CPU spec (sync) + spawn job entities + frame the camera
            WaitReady,   ///< poll MeshInstanceReadyComponent on every job entity
            WaitCapture, ///< poll readbackTargetAsync
            Encode,      ///< kick off the CPU encode (swizzle + multi-size PNG + decode) on the pool
            WaitEncode,  ///< poll the offloaded encode result, then push the display texture
            WaitDisplay  ///< poll createTexture2D (display texture)
        };

        lux::asset::asset_id_t id{};
        lux::asset::EAssetType type{lux::asset::EAssetType::UNKNOWN};
        Stage                  stage{Stage::Spec};

        // Watchdog state: a default-constructed RenderRequest reports
        // isReady()==false forever, so settledness checks must only look at
        // requests that were actually issued (the *_issued flags).
        int  age_frames{0};
        bool capture_issued{false};
        bool display_issued{false};
        // One-shot fallback: a readback that comes back literally empty swaps
        // the authored material for PreviewGrey and re-renders once.  A second
        // empty capture is a failed thumbnail, never a cacheable black image.
        bool retried_default{false};

        ThumbnailSpec spec;
        bool          from_embedded{false};
        ThumbnailSet  set;

        /// The job's world entities (one per spec instance). Their whole GPU
        /// footprint — mesh/material upload, instance, refcounts — is owned by
        /// the resolver + mesh subsystem; destroying the entities reclaims it.
        std::vector<lux::meta::entity_id> entities;

        lux::render::RenderRequest<lux::render::ReadbackTargetReply>   capture_req;
        lux::render::RenderRequest<lux::render::Texture2DCreatedReply> display_req;

        std::vector<std::byte> dst_px;     ///< readback target (alive until capture resolves)
        std::vector<std::byte> display_px; ///< display-tex pixels (alive until display resolves)
        lux::cxx::SharedBytes<> display_bytes;
        std::uint32_t          display_w{0};
        std::uint32_t          display_h{0};

        // Offloaded-encode handoff (Encode → WaitEncode). The pool op + the Job share it.
        std::shared_ptr<EncodeOut> encode_out;

        lux::render::RTextureHandle result_tex{};
    };

    namespace
    {
        // (手工 preview pack / 相机 look-at 基 / parseBuiltinId 已提取到
        //  PreviewWorldCommon —— MaterialPreviewHost 与本服务共用,批 2。)

        /// frameBounds 数学(轨道公式沿自旧手写预览的 frameBounds):把「取景这个
        /// AABB」写成相机实体的数据 —— 位姿进 Transform3D(aimPreviewCamera),
        /// fov/near/far 进 Camera3D。Camera3DSystem 用同一个 TLookAt/TPerspective
        /// 帮手推导矩阵,所以像旧实现一样取景。
        void frameCameraForBounds(lux::ecs::World&           world,
                                  lux::meta::entity_id       camera,
                                  const lux::math::AABB&     bounds)
        {
            const Eigen::Vector3f center = bounds.center();
            float radius = bounds.extents().norm() * 0.5f; // bounding-sphere radius
            if (!(radius > 1e-4f)) radius = 0.5f;          // degenerate / empty guard

            const float fov  = 45.f * 3.14159265f / 180.f;
            const float dist = (radius / std::sin(fov * 0.5f)) * 1.25f; // + margin
            const Eigen::Vector3f dir = Eigen::Vector3f(0.6f, 0.45f, 1.0f).normalized();
            const Eigen::Vector3f eye = center + dir * dist;

            aimPreviewCamera(world, camera, eye, center);

            auto& cc = world.registry().get<lux::ecs::Camera3DComponent>(camera);
            cc.fov_rad     = fov;
            cc.near_z      = std::max(0.01f, dist - radius * 2.f);
            cc.far_z       = dist + radius * 2.f + 1.f;
        }

        // RGBA8 → a multi-size PNG ThumbnailSet (1:1 copy at native size, sRGB
        // box-downscale otherwise).
        ThumbnailSet buildSetFromRgba(const std::byte* rgba, std::uint32_t w, std::uint32_t h,
                                      std::span<const ThumbnailSize> sizes)
        {
            ThumbnailSet set;
            for (const auto sz : sizes)
            {
                if (sz.width == 0 || sz.height == 0) continue;
                std::vector<std::byte> scaled;
                if (sz.width == w && sz.height == h)
                    scaled.assign(rgba, rgba + static_cast<std::size_t>(w) * h * 4);
                else
                    scaled = downscaleRgba8(rgba, w, h, sz.width, sz.height);
                if (scaled.empty()) continue;

                ThumbnailImage img;
                img.width = sz.width; img.height = sz.height;
                img.encoding = EThumbnailEncoding::PNG;
                img.bytes = encodePngRgba8(scaled.data(), sz.width, sz.height);
                if (!img.bytes.empty())
                    set.images.push_back(std::move(img));
            }
            return set;
        }
    } // namespace

    ThumbnailService::ThumbnailService(lux::asset::AssetManager& assets,
                                       lux::render::RenderFrameSession& session,
                                       const EditorRenderInfra& render_infra,
                                       lux::asset_runtime::AssetClient asset_client,
                                       lux::exec::AsyncRuntime& async)
        : assets_(assets), session_(session), infra_(render_infra),
          asset_client_(std::move(asset_client)), async_(async),
          cache_(std::make_unique<CacheImpl>()),
          async_scope_(std::make_unique<lux::exec::AsyncScope>(async))
    {
    }

    ThumbnailService::~ThumbnailService()
    {
        // 自己收自己的尾。正常路径 LuxEditor 先 releaseGpu()(渲染线程还活着)再
        // shutdown()(线程停了);漏调时这里兜底 —— shutdown() 全是 reset/clear,
        // 幂等,而 host_ 成员析构会带倒 SceneRuntime(其 tearDown 对已停通道的
        // 提交返回 false,静默放弃,GPU 侧由 device-destroy 回收)。
        shutdown();
    }

    bool ThumbnailService::initialize(std::uint32_t render_size)
    {
        if (ready_) return true;

        auto host = std::make_unique<RuntimeHost>();
        host->render_size = render_size ? render_size : 256u;
        if (!parseBuiltinId(
                lux::asset::kBuiltinSphereMeshIdStr,
                host->sphere_mesh_id))
            return false;   // programmer error — the literal is compile-time
        // Mesh-instance creation requires a real material handle; nil is not a
        // renderer-default sentinel. Prefer PreviewGrey and fall back to the
        // builtin white material, but never let a thumbnail job reach the
        // render bridge with an invalid material configuration.
        (void)parseBuiltinId(
            lux::asset::kBuiltinPreviewGreyMaterialIdStr,
            host->preview_grey_id);
        if (!assets_.hasAsset(host->preview_grey_id))
        {
            (void)parseBuiltinId(
                lux::asset::kBuiltinWhitePbrMaterialIdStr,
                host->preview_grey_id
            );
            if (!assets_.hasAsset(host->preview_grey_id))
            {
                std::fprintf(
                    stderr,
                    "[Thumbnail] no builtin fallback material is registered — "
                    "thumbnails stay off\n"
                );
                return false;
            }
        }

        // ── HOST step 1: the offscreen target the thumbnail view composes onto.
        //    OURS to create and (in releaseGpu) destroy — the runtime only
        //    composes the camera's view onto it. SAMPLED 形态与 EditorScene 的
        //    主视口 target 同款(显示路径要可采样;readback 在两种形态上都走
        //    readbackTarget* 命令面)。
        const lux::common::Size2D extent{host->render_size, host->render_size};
        auto target_result = infra_.control->syncCall(
            infra_.control->createOffscreenRenderTarget(
                extent,
                lux::render::kTargetFlagSampled
            )
        );
        if (!target_result || !target_result->target.isValid())
        {
            std::fprintf(stderr, "[Thumbnail] createOffscreenRenderTarget failed — thumbnails stay off\n");
            return false;
        }
        const auto target_reply = *target_result;
        host->target = infra_.control->adoptTarget(target_reply.target);

        // ── HOST step 2: the preview SceneRuntime — same bring-up seam as the
        //    editor's main scene, with the manual preview pack + preview profile.
        if (!host->contributions.add(makePreviewWorldContribution()))
            return false;

        lux::runtime::SceneRuntime::Config rcfg;
        rcfg.name            = "Thumbnail";
        rcfg.transient_package = makePreviewScenePackage(rcfg.name);
        rcfg.events          = infra_.events;      // 进程域同一个 bus(批B,可空)
        // ★ 批 D2:守卫在这里,因为 `RenderInfra::residency` 按设计可空 —— 详见
        //   `EditorScene::bringUp` 同位置的说明。
        if (infra_.residency == nullptr || infra_.control == nullptr ||
            infra_.components == nullptr ||
            !infra_.upload)
        {
            std::fprintf(stderr, "[Thumbnail] RenderInfra::residency is not wired — thumbnails stay off\n");
            return false;
        }
        lux::runtime::RenderSceneServices render_services{
            .frame           = session_,
            .control         = *infra_.control,
            .upload          = infra_.upload,
            .feature_catalog = infra_.feature_catalog,
            .feature_plan    = infra_.feature_plan,
            .residency       = *infra_.residency,
            .profile         = lux::runtime::previewProfile(),
            .render_effect_catalog = infra_.render_effect_catalog,
            .render_effect_types = infra_.render_effect_types,
        };
        const lux::runtime::SceneRuntime::Dependencies deps{
            .assets          = assets_,
            .asset_client    = asset_client_,
            .async           = async_,
            .components      = *infra_.components,
            .entity_sections = infra_.entity_sections,
            .scene_contribution_catalog = &host->contributions,
            .extension_modules = infra_.extension_modules,
        };
        host->runtime = lux::runtime::SceneRuntime::create(
            deps,
            rcfg,
            std::make_unique<lux::runtime::RenderSceneIntegration>(
                render_services,
                lux::runtime::RenderSceneConfig{
                    .target = host->target.id()}));
        if (!host->runtime)
        {
            std::fprintf(stderr, "[Thumbnail] preview SceneRuntime bring-up failed — thumbnails stay off\n");
            (void)host->target.close();
            return false;
        }

        // ── HOST step 3: resident entities — key light + framing camera
        //    (方图钉 1:1,auto_aspect=false;帮手见 PreviewWorldCommon)。
        auto& world = host->runtime->world();
        host->key_light = createPreviewKeyLight(world);
        host->camera    = createPreviewCamera(world, host->target.id(), extent,
                                               /*auto_aspect=*/false);
        // 第一帧提交之前 view 必须在位,否则 target 上没有任何层。
        lux::runtime::renderScene(*host->runtime)->settleViewCreation();

        host_      = std::move(host);
        providers_ = makeDefaultThumbnailSpecProviders();
        sizes_     = {{256, 256}, {128, 128}, {64, 64}};
        ready_     = true;
        return true;
    }

    ImTextureID ThumbnailService::requestThumbnail(const lux::asset::asset_id_t& id,
                                                   lux::asset::EAssetType type)
    {
        if (!ready_) return ImTextureID{0};

        auto& cache = cache_->cache;
        if (const auto* v = cache.tryGet(id)) return v->im_id;             // Ready
        if (cache.state(id) == lux::exec::CacheState::Failed) return ImTextureID{0};
        if (cache.markPending(id))                                         // first sighting -> enqueue
            queue_.push_back({id, type});
        return ImTextureID{0};
    }

    const ThumbnailSet* ThumbnailService::readySet(const lux::asset::asset_id_t& id) const
    {
        const auto* v = cache_->cache.tryGet(id);
        return v ? &v->set : nullptr;
    }

    void ThumbnailService::tick()
    {
        if (!ready_) return;
        // Drive the preview world one frame FIRST (frame OPEN): the resolver
        // uploads, the mesh subsystem runs the instance lifecycle and plants
        // MeshInstanceReadyComponent — the job state machine below observes it.
        host_->runtime->tick(1.f / 60.f,
                             static_cast<float>(host_->render_size),
                             static_cast<float>(host_->render_size),
                             host_->mapper);
        drainZombies();
        if (!active_) startNextJob();
        if (active_)
        {
            advanceJob();
            // Watchdog AFTER the step: a wait stage whose replies never arrive
            // must not starve the single-job queue forever (the failure mode
            // that blanked the whole grid).
            if (active_ && ++active_->age_frames > kJobFrameBudget)
                abortStuckJob();
        }
    }

    void ThumbnailService::startNextJob()
    {
        while (!queue_.empty())
        {
            const auto [id, type] = queue_.front();
            queue_.erase(queue_.begin());

            if (cache_->cache.state(id) != lux::exec::CacheState::Pending)
                continue;   // already resolved / invalidated since enqueue

            auto job  = std::make_unique<Job>();
            job->id   = id;
            job->type = type;

            // Embedded-payload fast path: an asset that already carries a
            // thumbnail set skips rendering — only the display-texture upload runs.
            if (const auto* asset = assets_.fetchAsset(id))
                if (auto emb = readThumbnailSet(*asset); emb && !emb->empty())
                {
                    job->set           = std::move(*emb);
                    job->from_embedded = true;
                    job->stage         = Job::Stage::Encode; // Encode skips build, pushes display
                }

            // A type with no spec provider (and no embedded payload) can never
            // produce a thumbnail — fail the entry QUIETLY instead of churning
            // a doomed job through the queue (SKELETON / ANIMATION_CLIP land
            // here today; the grid shows their type glyph, which is correct).
            if (!job->from_embedded && providers_.get(job->type) == nullptr)
            {
                cache_->cache.setFailed(id);
                continue;
            }

            active_ = std::move(job);
            return;
        }
    }

    void ThumbnailService::advanceJob()
    {
        Job& j = *active_;
        switch (j.stage)
        {
        case Job::Stage::Spec:
        {
            auto* r = providers_.get(j.type);
            // W2b: provider requests an async load for any data-less shell it needs.
            const ThumbnailLoadFn load = [this](const lux::asset::asset_id_t& dep)
            { (void)asset_client_.request(dep); };
            j.spec = r ? r->buildSpec(assets_, host_->sphere_mesh_id, j.id, load)
                       : ThumbnailSpec{};
            if (!j.spec.valid)
            {
                // Deps still streaming in → re-queue (cache stays Pending) instead
                // of failing permanently; bounded so a never-arriving dep fails.
                if (j.spec.pending && ++pending_attempts_[j.id] <= kPendingRetryBudget)
                {
                    queue_.push_back({j.id, j.type});
                    active_.reset();
                    return;
                }
                pending_attempts_.erase(j.id);
                finishJob(false);
                return;
            }
            pending_attempts_.erase(j.id);   // spec ready — done waiting on deps

            if (j.spec.has_cpu_pixels) { j.stage = Job::Stage::Encode; return; } // texture → encode

            // 3D: one job ENTITY per instance — MeshComponent{ids} is the whole
            // upload story (resolver + mesh subsystem take it from here).
            auto& world = host_->runtime->world();
            for (const auto& in : j.spec.instances)
            {
                if (in.mesh_asset_id.is_nil()) continue;
                const auto e = world.createEntity();
                world.emplace<lux::ecs::Transform3DComponent>(e);
                world.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
                auto& mc = world.emplace<lux::ecs::MeshComponent>(e);
                mc.mesh_asset_id     = in.mesh_asset_id;
                // 没有作者材质 → 预览灰(观感与旧手写预览场景的默认灰一致);
                // 预览灰缺席则留 nil(渲染侧默认材质)。
                mc.material_asset_id = in.material_asset_id.is_nil()
                                         ? host_->preview_grey_id
                                         : in.material_asset_id;
                mc.cast_shadow       = true;
                mc.visible           = true;
                j.entities.push_back(e);
            }
            if (j.entities.empty()) { finishJob(false); return; }

            frameCameraForBounds(world, host_->camera, j.spec.bounds);
            j.stage = Job::Stage::WaitReady;
            return;
        }

        case Job::Stage::WaitReady:
        {
            // 批 0 的观察点:实例建成(slot 回复落地)且对当前 view 代次发过可见性。
            // 材质就绪由 resolver 隐含 —— MeshGpuCacheComponent 在 = 两者都可用,
            // 实例建起来即材质路径已通。
            auto& reg = host_->runtime->world().registry();
            for (const auto e : j.entities)
                if (!reg.valid(e)
                    || !reg.all_of<lux::ecs::MeshInstanceReadyComponent>(e))
                    return;   // keep waiting (the watchdog bounds this)

            const std::uint32_t rs = host_->render_size;
            j.dst_px.assign(static_cast<std::size_t>(rs) * rs * 4, std::byte{0});
            j.capture_req = infra_.control->readbackTargetAsync(
                host_->target.id(), j.dst_px.data(), j.dst_px.size(), kSettleFrames);
            j.capture_issued = true;
            j.stage = Job::Stage::WaitCapture;
            return;
        }

        case Job::Stage::WaitCapture:
        {
            if (!j.capture_req.isReady()) return;
            const auto rb = j.capture_req.tryResult()->get();
            if (rb.status != 0 || rb.bytes_written != j.dst_px.size())
            {
                finishJob(false);
                return;
            }
            // Empty-image detection (see retried_default): when the render
            // produced literally nothing, replace the authored material with
            // PreviewGrey and re-render once. Nil is not a renderer-default
            // sentinel; it is an invalid mesh-instance configuration. The swap rides the standard
            // path: the resolver re-resolves, the mesh subsystem tears the
            // instance down + rebuilds, Ready re-arms — WaitReady re-waits.
            if (!j.spec.has_cpu_pixels)
            {
                bool any_nonblack = false;
                for (std::size_t i = 0; i + 2 < j.dst_px.size(); i += 4)
                    if (j.dst_px[i] != std::byte{0}
                        || j.dst_px[i + 1] != std::byte{0}
                        || j.dst_px[i + 2] != std::byte{0})
                    {
                        any_nonblack = true;
                        break;
                    }

                if (!any_nonblack && !j.retried_default && !j.entities.empty())
                {
                    j.retried_default = true;
                    auto& reg = host_->runtime->world().registry();
                    const auto fallback_material = host_->preview_grey_id;
                    for (const auto e : j.entities)
                        if (reg.valid(e))
                            reg.patch<lux::ecs::MeshComponent>(
                                e,
                                [fallback_material](lux::ecs::MeshComponent& mesh)
                                {
                                    mesh.material_asset_id = fallback_material;
                                }
                            );
                    j.capture_issued = false;
                    j.capture_req    = {};
                    j.stage = Job::Stage::WaitReady;
                    return;
                }
                if (!any_nonblack)
                {
                    std::fprintf(
                        stderr,
                        "[Thumbnail] type=%d authored and PreviewGrey "
                        "captures were empty; refusing to cache a black "
                        "thumbnail\n",
                        static_cast<int>(j.type));
                    finishJob(false);
                    return;
                }
            }
            j.stage = Job::Stage::Encode; // encode next tick (keeps per-tick work small)
            return;
        }

        case Job::Stage::Encode:
        {
            // Offload the PNG work (swizzle + multi-size encode + decode) to the CPU pool
            // so it no longer hitches the editor's main thread. Snapshot/MOVE every input
            // the work needs into the op so it owns them — the op never touches the Job or
            // the session (createTexture2D is issued back on the main thread in WaitEncode),
            // so an abort/shutdown can simply orphan it.
            auto out = std::make_shared<EncodeOut>();
            j.encode_out = out;

            const int           mode = j.from_embedded ? 0 : (j.spec.has_cpu_pixels ? 1 : 2);
            const std::uint32_t rs   = host_ ? host_->render_size : 256u;
            const std::uint32_t disp = kDisplaySize;
            std::vector<ThumbnailSize> sizes = sizes_;            // small copy
            std::vector<std::byte>     px;                        // moved input pixels
            std::uint32_t              w = 0, h = 0;
            ThumbnailSet               embedded;
            if      (mode == 0) embedded = std::move(j.set);                                    // already built
            else if (mode == 1) { px = std::move(j.spec.rgba8); w = j.spec.cpu_width; h = j.spec.cpu_height; }
            else                { px = std::move(j.dst_px);      w = rs;               h = rs; }

            const bool started = lux::exec::spawn(*async_scope_,
                  ::stdexec::schedule(
                      lux::exec::backgroundCpuScheduler(async_))
                | ::stdexec::then([out, mode, w, h, disp, sizes = std::move(sizes),
                                   px = std::move(px), embedded = std::move(embedded)]() mutable noexcept
                  {
                      // Business failures are values (`empty` / `nullopt`).
                      // Allocation failure follows the engine's fatal OOM
                      // policy; exceptions are not a second control channel.
                      ThumbnailSet set;
                      if      (mode == 0) set = std::move(embedded);
                      else if (mode == 1) set = buildSetFromRgba(px.data(), w, h, sizes);
                      else
                      {
                          // GPU readback is BGRA + forward leaves the lit object at alpha 0
                          // → swizzle to RGBA and force opaque.
                          swizzleBgraRgba(px.data(), static_cast<std::size_t>(w) * h);
                          for (std::size_t i = 3; i < px.size(); i += 4) px[i] = std::byte{255};
                          set = buildSetFromRgba(px.data(), w, h, sizes);
                      }
                      if (set.empty())
                      { out->failed = true; out->done.store(true, std::memory_order_release); return; }

                      const ThumbnailImage* best = set.best(disp, disp);
                      std::optional<DecodedImage> decoded;
                      if (best) decoded = decodePngRgba8(best->bytes);
                      if (!decoded)
                      { out->failed = true; out->done.store(true, std::memory_order_release); return; }

                      out->set        = std::move(set);
                      out->display_w  = decoded->width;
                      out->display_h  = decoded->height;
                      out->display_px = std::move(decoded->rgba8);
                      out->done.store(true, std::memory_order_release);
                  })
                | ::stdexec::upon_stopped([out]() noexcept     // owner/executor shutdown cancel
                  { out->failed = true; out->done.store(true, std::memory_order_release); }));

            if (!started)
            {
                out->failed = true;
                out->done.store(true, std::memory_order_release);
            }

            j.stage = Job::Stage::WaitEncode;
            return;
        }

        case Job::Stage::WaitEncode:
        {
            if (!j.encode_out || !j.encode_out->done.load(std::memory_order_acquire)) return;
            EncodeOut& eo = *j.encode_out;
            if (eo.failed) { finishJob(false); return; }

            j.set        = std::move(eo.set);
            j.display_w  = eo.display_w;
            j.display_h  = eo.display_h;
            j.display_px = std::move(eo.display_px);
            auto display_owner =
                std::make_shared<std::vector<std::byte>>(
                    std::move(j.display_px));
            auto retained = lux::cxx::SharedBytes<>::fromOwner(
                display_owner,
                std::span<const std::byte>{
                    display_owner->data(), display_owner->size()});
            if (retained.empty())
            {
                finishJob(false);
                return;
            }
            j.display_bytes = std::move(retained);
            j.encode_out.reset();      // drop our ref to the (now consumed) EncodeOut

            j.stage = Job::Stage::WaitDisplay;
            return;
        }

        case Job::Stage::WaitDisplay:
        {
            if (!j.display_issued)
            {
                auto submitted = infra_.upload.tryCreateTexture2D(
                    j.display_bytes,
                    static_cast<std::int32_t>(j.display_w),
                    static_cast<std::int32_t>(j.display_h),
                    4,
                    lux::render::EPixelFormat::RGBA8_SRGB,
                    /*generate_mips=*/false
                );
                if (!submitted)
                {
                    if (submitted.error() ==
                            lux::render::ERenderUploadSubmitError::QUEUE_FULL ||
                        submitted.error() == lux::render::
                            ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED)
                        return;
                    finishJob(false);
                    return;
                }
                j.display_req = std::move(*submitted);
                j.display_issued = true;
            }
            if (!j.display_req.isReady()) return;
            const auto rep = j.display_req.tryResult()->get();
            // status 与 handle 双判:分发失败的默认回复 status==0 + 空句柄,只判
            // status 会把空纹理句柄当成功收进缩略图集。
            if (rep.status != 0 || rep.handle.isNull()) { finishJob(false); return; }
            j.result_tex = rep.handle;
            finishJob(true);
            return;
        }
        }
    }

    void ThumbnailService::invalidate(const lux::asset::asset_id_t& id)
    {
        // Ready entries own process-global display textures. Destruction is a
        // control-plane operation, so invalidation is legal from UI paint or
        // any other main-thread phase without a deferred frame queue.
        if (const auto* v = cache_->cache.tryGet(id); v && !v->gpu_tex.isNull())
            infra_.control->destroyTexture(v->gpu_tex);
        cache_->cache.invalidate(id);
        pending_attempts_.erase(id);   // 允许重新排队生成
    }

    void ThumbnailService::destroyJobEntities(Job& j)
    {
        if (host_ && host_->runtime && host_->runtime->isLive())
        {
            auto& world = host_->runtime->world();
            for (const auto e : j.entities)
                if (world.registry().valid(e))
                    world.destroyEntity(e);
            // 销毁即回收:leave 观察者记下实例句柄(下一次 tick removeMeshInstance),
            // 胶水松票,归零广播让驻留编排收 GPU 副本。这里没有
            // (也不再需要)任何手工 destroy。
        }
        j.entities.clear();
    }

    void ThumbnailService::finishJob(bool ok)
    {
        if (!active_) return;
        destroyJobEntities(*active_);
        if (!ok)
            std::fprintf(stderr, "[Thumbnail] FAILED type=%d at stage=%d\n",
                         static_cast<int>(active_->type),
                         static_cast<int>(active_->stage));
        if (ok)
        {
            // 重算覆盖旧条目(资产重导入后缩略图重生成):旧显示纹理无人再引用,
            // 不还就漏到进程结束。帧开着(advanceJob 语境),当场归还。
            if (const auto* old = cache_->cache.tryGet(active_->id);
                old && !old->gpu_tex.isNull() && !(old->gpu_tex == active_->result_tex))
                infra_.control->destroyTexture(old->gpu_tex);
            ThumbValue v;
            v.set     = std::move(active_->set);
            v.gpu_tex = active_->result_tex;
            v.im_id   = lux::ui::encodeTextureHandleSentinel(active_->result_tex);
            cache_->cache.setReady(active_->id, std::move(v));
        }
        else
        {
            cache_->cache.setFailed(active_->id);
        }
        active_.reset();
    }

    // ── Watchdog: skip a job whose waits never settle ─────────────────────────
    void ThumbnailService::abortStuckJob()
    {
        Job& j = *active_;

        // Account WHICH wait is holding the job — this line pins the root
        // cause when the watchdog fires in the field.
        std::size_t not_ready = 0;
        if (host_ && host_->runtime && host_->runtime->isLive())
        {
            auto& reg = host_->runtime->world().registry();
            for (const auto e : j.entities)
                if (!reg.valid(e)
                    || !reg.all_of<lux::ecs::MeshInstanceReadyComponent>(e))
                    ++not_ready;
        }
        std::fprintf(stderr,
            "[Thumbnail] job STUCK type=%d stage=%d after %d frames "
            "(instances not ready=%zu capture_pending=%d display_pending=%d) — "
            "skipped, queue continues\n",
            static_cast<int>(j.type), static_cast<int>(j.stage), j.age_frames,
            not_ready,
            static_cast<int>(j.capture_issued && !j.capture_req.isReady()),
            static_cast<int>(j.display_issued && !j.display_req.isReady()));

        // The job's GPU footprint is entity-owned — destroying the entities
        // reclaims it through the standard leave/refcount path.
        destroyJobEntities(j);
        cache_->cache.setFailed(j.id);

        // Only an outstanding readback / display upload still writes into
        // job-owned memory (dst_px / display_px) — park the Job until those
        // settle. Everything else (encode_out) is self-contained and orphaned.
        const bool outstanding =
            (j.capture_issued && !j.capture_req.isReady()) ||
            (j.display_issued && !j.display_req.isReady());
        if (outstanding)
        {
            zombies_.push_back(std::move(active_));
            return;
        }
        // A display texture that resolved but was never harvested would leak —
        // unreachable today (advanceJob runs before the watchdog), kept as a belt.
        if (j.display_issued && j.display_req.isReady())
            if (const auto rep = j.display_req.tryResult()->get();
                rep.status == 0 && !rep.handle.isNull())   // 默认回复的空句柄不 destroy
                infra_.control->destroyTexture(rep.handle);
        active_.reset();
    }

    void ThumbnailService::drainZombies()
    {
        for (std::size_t z = 0; z < zombies_.size(); )
        {
            Job& j = *zombies_[z];
            // Only the readback + display requests gate settledness now — the
            // entity path has no client-side buffers a late reply could touch.
            const bool settled =
                (!j.capture_issued || j.capture_req.isReady())
                && (!j.display_issued || j.display_req.isReady());
            if (!settled)
            {
                ++z;
                continue;
            }
            // Free a display texture that resolved AFTER the abort.
            if (j.display_issued)
                if (const auto rep = j.display_req.tryResult()->get();
                    rep.status == 0 && !rep.handle.isNull())   // 默认回复的空句柄不 destroy
                    infra_.control->destroyTexture(rep.handle);
            zombies_.erase(zombies_.begin() + static_cast<std::ptrdiff_t>(z));
        }
    }

    bool ThumbnailService::releaseGpu()
    {
        if (!host_) return true;
        if (async_scope_)
        {
            if (infra_.close_driver == nullptr ||
                !infra_.close_driver->close(async_scope_->closeAsync()))
            {
                lux::log::error(
                    "thumbnail",
                    "thumbnail async close timed out; dependencies stay alive");
                return false;
            }
            async_scope_.reset();
        }
        // Park an in-flight readback/display (its job-owned buffers must outlive
        // the server; shutdown() reaps after the thread stops). The job's
        // entities die with the runtime teardown below.
        if (active_)
        {
            cache_->cache.setFailed(active_->id);
            active_->entities.clear();   // world dies wholesale below
            const bool outstanding =
                (active_->capture_issued && !active_->capture_req.isReady()) ||
                (active_->display_issued && !active_->display_req.isReady());
            if (outstanding) zombies_.push_back(std::move(active_));
            else             active_.reset();
        }
        // 成功 job 的显示纹理(readback→重上传的产物,非资产派生物,不归共享
        // 缓存管):此前 shutdown 只 clear 表,句柄落地即漏 —— 浏览 N 个资产 =
        // N 张 RGBA8 常驻到进程结束。在渲染线程还活着、帧开着的这里逐个归还。
        cache_->cache.forEachReady(
            [this](const lux::asset::asset_id_t&, const ThumbValue& v)
            {
                if (!v.gpu_tex.isNull())
                    infra_.control->destroyTexture(v.gpu_tex);
            });
        cache_->cache.clear();
        if (host_->runtime)
        {
            if (infra_.close_driver == nullptr)
            {
                lux::log::error("thumbnail", "preview has no MainCloseDriver");
                return false;
            }
            const auto report = infra_.close_driver->close(*host_->runtime);
            if (!report)
            {
                lux::log::error("thumbnail", "preview scene close timed out");
                return false;
            }
            host_->runtime.reset();
        }
        if (host_->target)
        {
            // Ours to destroy (we created it). SAMPLED 池经统一退休入口回收。
            if (auto closing = host_->target.close(); closing)
            {
                (void)infra_.control->syncCall(std::move(*closing));
            }
        }
        ready_ = false;
        return true;
    }

    void ThumbnailService::shutdown()
    {
        // The normal path closed this scope in releaseGpu while MainThreadMailbox and
        // render progress were still available. Destruction only requests stop
        // as a passive fallback; it never runs a private blocking pump.
        if (async_scope_)
            async_scope_->requestStop();

        // The render thread must already be stopped before this runs, so the
        // server can no longer write into an in-flight readback's dst buffer or
        // a pending display upload's borrowed pixels.
        active_.reset();
        zombies_.clear();
        cache_->cache.clear();
        queue_.clear();
        pending_attempts_.clear();
        ready_ = false;
    }

} // namespace lux::editor
