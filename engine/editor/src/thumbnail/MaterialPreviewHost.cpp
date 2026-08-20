/**
 * @file MaterialPreviewHost.cpp
 *
 * 活材质预览的第二个 SceneRuntime(装配归属 ADR 工作线三批 2)。形状与
 * ThumbnailService 的 RuntimeHost 同款(共用 PreviewWorldCommon 的装配小件);
 * 业务差异全在「换刀」:
 *
 *   面板 compile → MaterialData(授权图 clone + 双份 SPIR-V + 贴图槽 UUID)
 *     → setGraphContent 暂存(latest-wins)
 *     → tick:注册临时 in-memory MaterialAsset(new UUID,不落盘)
 *       + patch 球实体 material_asset_id → 新 id
 *     → MeshResolver:ensureGraphMaterial(compile 双 frag → 解析贴图 →
 *       uploadGraphMaterial,逐材质 forward PSO)…就绪后写 MeshGpuCacheComponent
 *       并 release 旧材质引用
 *     → MeshInstanceSubsystem:material_source 变了 → 实例拆掉,下一帧重建
 *     → 旧临时资产:归零广播 → 驻留编排收 GPU 副本;
 *       本宿主(自己也是一个广播消费者)看到自己名下的 id 归零,把
 *       AssetManager 侧的 CPU shell removeAsset 掉 —— 临时资产全生命周期闭环。
 *
 * 参数拖动不换刀:modifyGraphMaterial 直呼球实体当前的 GPU 材质句柄
 * (MeshGpuCacheComponent.material)。只对**自己注册的临时材质**发 —— 首刀
 * 就绪前球穿的是共享的 PreviewGrey,对它 modify 会把缩略图等别处的灰球一起
 * 改色。语义「最后一次赢」:换刀在途时新参数盖旧参数,换刀就绪那一刻再补发
 * 一次(上传载荷本身带的是编译时刻的图默认值,期间的拖动要补上)。
 */
#include "thumbnail/MaterialPreviewHost.hpp"
#include <lux/engine/runtime/frame/MainCloseDriver.hpp>
#include "thumbnail/PreviewWorldCommon.hpp"

#include <lux/engine/editor/app/LuxEditor.hpp>            // EditorRenderInfra
#include <lux/engine/resource/asset/BuiltinAssetIds.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/AssetEvents.hpp>   // AssetUnreferenced(批E 订阅)
#include <lux/engine/resource/asset/MaterialAsset.hpp>
#include <lux/engine/ecs/render/RenderResourceEvents.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>          // 进程域驻留三件套(贴图槽解析)
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>         // RTextureHandle(位解包)
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>  // MaterialOperationIds / MaterialProxy / modifyGraphMaterial
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>

#include <lux/engine/runtime/scene/SceneRuntime.hpp>
#include <lux/engine/runtime/render/scene/RenderSceneIntegration.hpp>
#include <lux/engine/runtime/render/scene/StandardFeaturePlan.hpp>   // previewProfile

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/render/components/3d/MeshComponent.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/render/components/MeshGpuCacheComponent.hpp>
#include <lux/engine/ecs/render/components/MeshInstanceReadyComponent.hpp>
#include <lux/engine/ecs/render/components/ViewPresentComponent.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>

#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/math/Extent.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <uuid.h>   // std::hash<uuids::uuid>(retire/owned 集合)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace lux::editor
{
    // ── The private preview SceneRuntime + the swap state machine ────────────
    // The private catalogue owns only the preview descriptor. mapper is empty:
    // preview worlds have no input.
    struct MaterialPreviewHost::Host
    {
        lux::runtime::SceneContributionCatalog      contributions;
        lux::input::ActionMapper                    mapper;
        std::unique_ptr<lux::runtime::SceneRuntime> runtime;
        lux::render::RenderTargetLease              target{};
        lux::meta::entity_id                        camera{entt::null};
        lux::meta::entity_id                        key_light{entt::null};
        lux::meta::entity_id                        sphere{entt::null};
        lux::asset::asset_id_t                      sphere_mesh_id{};
        lux::asset::asset_id_t                      preview_grey_id{};
        lux::render::MaterialOperationIds           material_ops{};
        std::uint32_t                               cur_w{512}, cur_h{512};

        // ── 换刀状态机(single-flight + latest-wins)────────────────────────
        std::unique_ptr<lux::asset::MaterialData>   pending_payload;  ///< 最新一份待换的内容
        lux::asset::asset_id_t                      current_id{};     ///< 球此刻想穿的材质
        lux::asset::asset_id_t                      replacing_id{};   ///< 换刀在途时被顶替的那个(watchdog 回退目标)
        bool                                        swap_in_flight{false};
        int                                         swap_frames{0};

        // ── 临时资产账本 ────────────────────────────────────────────────────
        /// 本宿主注册进 AssetManager、还没 removeAsset 的临时材质 shell。
        std::unordered_set<lux::asset::asset_id_t>  owned_temp_ids;
        /// 已被顶替、等归零广播确认(resolver 松手)后再 removeAsset 的那些。
        std::unordered_set<lux::asset::asset_id_t>  retire_on_unref;
        /// 归零事件送达、待 tick 步骤3 收 shell 的 id(批E:订阅只入队 ——
        /// removeAsset 动账本,统一在 tick 的安全点做)。
        std::vector<lux::asset::asset_id_t>         pending_unref;
        /// 本宿主的归零订阅(批E:懒建宿主 = 动态生命周期例外,自持订阅;
        /// 随本结构体析构退订,先于 infra 的 bus 亡)。
        lux::events::SubscriptionGroup              subs;

        // ── 参数(最后一次赢)──────────────────────────────────────────────
        lux::render::GraphMaterialData              params{};
        bool                                        params_valid{false};
        bool                                        params_dirty{false};

        // ── 轨道相机 + 改尺寸(与旧实现同参:温和的 3/4 视角)──────────────
        float         yaw{0.6f}, pitch{0.35f}, dist{2.8f};
        bool          pending_resize{false};
        std::uint32_t pend_w{0}, pend_h{0};
    };

    MaterialPreviewHost::MaterialPreviewHost(lux::asset::AssetManager&   assets,
                                             lux::render::RenderFrameSession& session,
                                             const EditorRenderInfra&    render_infra,
                                             lux::asset_runtime::AssetClient asset_client,
                                             lux::exec::AsyncRuntime& async)
        : assets_(assets), session_(session), infra_(render_infra)
        , asset_client_(std::move(asset_client)), async_(async)
    {
    }

    MaterialPreviewHost::~MaterialPreviewHost()
    {
        // 正常路径 LuxEditor 先 releaseGpu()(渲染线程还活着)再 shutdown()
        // (线程停了);漏调时这里兜底 —— shutdown() 全是 CPU 侧清账,幂等,
        // 而 host_ 成员析构会带倒 SceneRuntime(其 tearDown 对已停通道的提交
        // 返回 false,静默放弃,GPU 侧由 device-destroy 回收)。
        shutdown();
    }

    lux::render::RenderTargetId MaterialPreviewHost::target() const noexcept
    {
        return host_ ? host_->target.id() : lux::render::RenderTargetId{};
    }

    bool MaterialPreviewHost::contentReady() const noexcept
    {
        if (!ready_ || !host_ || !host_->runtime ||
            !host_->runtime->isLive() || host_->swap_in_flight)
            return false;
        const Host& h = *host_;
        const auto& reg = h.runtime->world().registry();
        if (!reg.valid(h.sphere) ||
            !reg.all_of<lux::ecs::MeshInstanceReadyComponent>(h.sphere))
            return false;
        const auto* gpu = reg.try_get<lux::ecs::MeshGpuCacheComponent>(h.sphere);
        return gpu != nullptr && gpu->material_source == h.current_id;
    }

    bool MaterialPreviewHost::initialize(std::uint32_t render_size)
    {
        if (ready_) return true;

        auto host = std::make_unique<Host>();
        host->cur_w = host->cur_h = render_size ? render_size : 512u;
        if (!parseBuiltinId(
                lux::asset::kBuiltinSphereMeshIdStr,
                host->sphere_mesh_id))
            return false;   // programmer error — the literal is compile-time
        // 预览灰缺席不致命:球以 nil 材质起步(渲染侧默认),首刀落地后无差异。
        (void)parseBuiltinId(
            lux::asset::kBuiltinPreviewGreyMaterialIdStr,
            host->preview_grey_id);
        if (!assets_.hasAsset(host->preview_grey_id))
            host->preview_grey_id = {};

        // ── HOST step 1: SAMPLED offscreen target(面板经 ImGui target sentinel
        //    采样显示;非 SAMPLED 目标的 sentinel 解析为 VK_NULL_HANDLE)。
        //    OURS to create and (in releaseGpu) destroy。
        const lux::math::Extent2u extent{host->cur_w, host->cur_h};
        auto target_result = infra_.control->syncCall(
            infra_.control->createOffscreenRenderTarget(
                extent,
                lux::render::kTargetFlagSampled
            )
        );
        if (!target_result || !target_result->target.isValid())
        {
            std::fprintf(stderr, "[MaterialPreview] createOffscreenRenderTarget failed — live preview stays off\n");
            return false;
        }
        const auto target_reply = *target_result;
        host->target = infra_.control->adoptTarget(target_reply.target);

        // ── HOST step 2: the preview SceneRuntime — same bring-up seam as the
        //    thumbnail world (manual preview pack + preview profile).
        if (!host->contributions.add(makePreviewWorldContribution()))
            return false;

        lux::runtime::SceneRuntime::Config rcfg;
        rcfg.name            = "MaterialPreview";
        rcfg.transient_package = makePreviewScenePackage(rcfg.name);
        rcfg.events          = infra_.events;      // 进程域同一个 bus(批B,可空)
        // ★ 批 D2:守卫在这里,因为 `RenderInfra::residency` 按设计可空 —— 详见
        //   `EditorScene::bringUp` 同位置的说明。
        if (infra_.residency == nullptr || infra_.control == nullptr ||
            infra_.components == nullptr ||
            !infra_.upload)
        {
            std::fprintf(stderr, "[MaterialPreview] RenderInfra::residency is not wired — live preview stays off\n");
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
            std::fprintf(stderr, "[MaterialPreview] preview SceneRuntime bring-up failed — live preview stays off\n");
            if (auto closing = host->target.close(); closing)
            {
                (void)infra_.control->syncCall(
                    std::move(closing.value())
                );
            }
            return false;
        }

        // 参数直呼(modifyGraphMaterial)用的 op-id:进程域目录按名取,与
        // 材质域子服务上传用的是同一套(op-id 属 feature TYPE,不属场景)。
        host->material_ops =
            infra_.feature_catalog.ops<lux::render::MaterialOperationIds>("StandardMaterial");

        // ── HOST step 3: resident entities — key light + orbit camera + sphere.
        auto& world = host->runtime->world();
        host->key_light = createPreviewKeyLight(world);
        // 相机 aspect 跟随 ViewPresent extent(auto_aspect):面板改尺寸时只要
        // patch extent,CameraViewSubsystem::syncAspect 每帧对齐。
        host->camera = createPreviewCamera(
            world, host->target.id(), extent, /*auto_aspect=*/true);

        host->sphere = world.createEntity();
        world.emplace<lux::ecs::Transform3DComponent>(host->sphere);
        world.emplace<lux::ecs::ResolvedTransform3DComponent>(host->sphere);
        {
            auto& mc = world.emplace<lux::ecs::MeshComponent>(host->sphere);
            mc.mesh_asset_id     = host->sphere_mesh_id;
            mc.material_asset_id = host->preview_grey_id;   // 首刀落地前的底色
            mc.cast_shadow       = true;   // 预览有自己的小阴影图集(自投影)
            mc.visible           = true;
        }
        host->current_id   = host->preview_grey_id;
        host->replacing_id = host->preview_grey_id;

        // 第一帧提交之前 view 必须在位,否则 target 上没有任何层。
        lux::runtime::renderScene(*host->runtime)->settleViewCreation();

        // 归零订阅(批E):临时材质被 resolver 松手归零时,把 CPU shell 收掉。
        // handler 只入队(pending_unref),removeAsset 在 tick 步骤3 的安全点做;
        // 别人的 id 也会送达 —— retire_on_unref 比对后忽略即可。
        if (infra_.events && infra_.frame_pump)
            host->subs.add(infra_.events->subscribe<lux::asset::AssetUnreferenced>(
                *infra_.frame_pump,
                [h = host.get()](const lux::asset::AssetUnreferenced& e)
                { h->pending_unref.push_back(e.id); }));

        host_  = std::move(host);
        ready_ = true;
        return true;
    }

    // ── Record-only API(面板 paint 期、帧 CLOSED 时可调)─────────────────────

    void MaterialPreviewHost::setGraphContent(std::unique_ptr<lux::asset::MaterialData> payload)
    {
        if (!ready_ || !payload) return;
        host_->pending_payload = std::move(payload);   // latest wins
    }

    void MaterialPreviewHost::updateGraphParams(const lux::render::GraphMaterialData& params)
    {
        if (!ready_) return;
        host_->params       = params;
        host_->params_valid = true;
        host_->params_dirty = true;
    }

    std::uint32_t MaterialPreviewHost::resolveTextureIndex(const lux::asset::asset_id_t& id)
    {
        // paint 期(帧关)安全:peek 是纯查表;真正的 request 由 tick() 代发。
        // 未上账是刻意的:面板预览贴图与材质槽位共用同一张表行,材质依赖门
        // 会正常取票;纯预览期的行由宿主关停力扫收(teardown)。
        if (id.is_nil() || !infra_.residency) return 0;
        const auto bits = infra_.residency->peekReadyBits(id);
        if (bits != 0)
        {
            pending_textures_.erase(id);
            return lux::ecs::unpackHandleBits<lux::render::RTextureHandle>(bits)
                .index;
        }
        pending_textures_.insert(id);
        return 0;
    }

    void MaterialPreviewHost::orbit(float d_yaw, float d_pitch, float d_zoom)
    {
        if (!ready_) return;
        Host& h = *host_;
        h.yaw  += d_yaw;
        h.pitch = std::clamp(h.pitch + d_pitch, -1.5f, 1.5f);
        h.dist  = std::clamp(h.dist * std::exp(-d_zoom), 1.0f, 8.0f);
    }

    void MaterialPreviewHost::requestResize(std::uint32_t width, std::uint32_t height)
    {
        if (!ready_ || width == 0 || height == 0) return;
        Host& h = *host_;
        h.pend_w = width; h.pend_h = height; h.pending_resize = true;
    }

    // ── The frame-OPEN tick ──────────────────────────────────────────────────

    void MaterialPreviewHost::tick()
    {
        if (!ready_) return;
        Host& h    = *host_;
        auto& world = h.runtime->world();
        auto& reg   = world.registry();

        // 1. 尺寸 + 相机位姿先写(CPU 数据),本帧的世界 tick 就吃到:resize 的
        //    ViewPresent patch 经观察者入队,CameraViewSubsystem 在 pre-world-tick
        //    排空并重设 layer;aspect 由 syncAspect 随 extent 对齐。
        if (h.pending_resize)
        {
            const lux::math::Extent2u extent{h.pend_w, h.pend_h};
            // M2c:改尺寸直达渲染目标图像池(视图渲染尺寸随 binding 派生)。
            infra_.control->resizeTarget(h.target.id(), extent);
            // 走 patch:直接写字段不发 on_update(EnTT 契约),layer 不会重设。
            reg.patch<lux::ecs::ViewPresentComponent>(h.camera,
                [&](lux::ecs::ViewPresentComponent& v) { v.extent = extent; });
            h.cur_w = h.pend_w; h.cur_h = h.pend_h;
            h.pending_resize = false;
        }
        {
            // 轨道相机:球在原点、半径 0.5 → 包络球半径 √3/2(与旧实现同式)。
            const float r = 0.5f * std::sqrt(3.f);
            const Eigen::Vector3f dir(std::cos(h.pitch) * std::sin(h.yaw),
                                      std::sin(h.pitch),
                                      std::cos(h.pitch) * std::cos(h.yaw));
            aimPreviewCamera(world, h.camera, dir * h.dist, Eigen::Vector3f::Zero());
            auto& cc  = reg.get<lux::ecs::Camera3DComponent>(h.camera);
            cc.near_z = std::max(0.01f, h.dist - r * 2.f);
            cc.far_z  = h.dist + r * 2.f + 1.f;
        }

        // 1b. 面板 paint 期记名的贴图槽,在这里(帧 OPEN)代发 request。
        //     就绪的由 resolveTextureIndex 下次查表时清出集合;去重 = 行状态。
        if (infra_.residency)
        {
            for (const auto& tid : pending_textures_)
                infra_.residency->request(tid,
                                          lux::ecs::EResourceDomain::TEXTURE);
        }

        // 2. 推进预览世界一帧(帧 OPEN):resolver 上传、网格子系统跑实例生命周期。
        h.runtime->tick(1.f / 60.f,
                        static_cast<float>(h.cur_w),
                        static_cast<float>(h.cur_h),
                        h.mapper);

        // 3. 归零事件(批E:frame 泵订阅入队,这里消费):自己名下、已被顶替
        //    的临时材质归零 = resolver 已松手、GPU 副本已由各场景缓存回收 ——
        //    CPU shell 现在可以安全移除。广播是多订阅者的,别人的 id 也会出现
        //    在这里,忽略即可。
        for (const auto& id : h.pending_unref)
            if (h.retire_on_unref.erase(id) > 0)
            {
                assets_.removeAsset(id);
                h.owned_temp_ids.erase(id);
            }
        h.pending_unref.clear();

        // 4. 换刀状态机(single-flight + latest-wins)。
        if (h.swap_in_flight)
        {
            const auto* gpu = reg.try_get<lux::ecs::MeshGpuCacheComponent>(h.sphere);
            if (gpu && gpu->material_source == h.current_id)
            {
                // 新材质解析就绪(实例的拆重建由网格子系统自理)。被顶替的那份
                // 若是我们的临时资产,等它的归零广播再收 shell(步骤 3)。
                h.swap_in_flight = false;
                h.swap_frames    = 0;
                if (h.owned_temp_ids.contains(h.replacing_id))
                    h.retire_on_unref.insert(h.replacing_id);
                // 上传载荷带的是编译时刻的图默认值 —— 期间若有参数拖动,补发一次。
                if (h.params_valid)
                    h.params_dirty = true;
            }
            else if (++h.swap_frames > kSwapFrameBudget)
            {
                // Watchdog:运行期 compile/upload 失败(ensureGraphMaterial 把条目
                // 记成 known-bad)对本宿主是不可见的静默等待 —— 不设上限的话后续
                // 每一次编辑都被单飞门永远堵死。回退到被顶替的材质,丢掉卡死的
                // 临时资产(它从未被 retain,不会有归零广播;其缓存条目属未上账
                // 残留,由宿主关停序的力扫(teardown)统一回收)。
                std::fprintf(stderr,
                    "[MaterialPreview] content swap STUCK after %d frames — "
                    "reverting to the previous material\n", h.swap_frames);
                reg.patch<lux::ecs::MeshComponent>(h.sphere,
                    [&](lux::ecs::MeshComponent& mc) { mc.material_asset_id = h.replacing_id; });
                assets_.removeAsset(h.current_id);
                h.owned_temp_ids.erase(h.current_id);
                h.current_id     = h.replacing_id;
                h.swap_in_flight = false;
                h.swap_frames    = 0;
            }
        }
        if (!h.swap_in_flight && h.pending_payload)
        {
            // 注册临时 in-memory MaterialAsset(new UUID,不落盘)并给球换刀。
            auto asset = assets_.createAsset<lux::asset::MaterialAsset>(std::move(h.pending_payload));
            const lux::asset::asset_id_t id = asset->id();
            if (auto* mi = asset->mutableInfo())
            {
                static constexpr char kName[] = "M_PreviewLive";
                std::memcpy(mi->display_name, kName, sizeof(kName));
            }
            if (!assets_.registerAsset(std::move(asset)))
            {
                std::fprintf(stderr, "[MaterialPreview] temp material register failed — edit dropped\n");
            }
            else
            {
                h.owned_temp_ids.insert(id);
                h.replacing_id = h.current_id;
                h.current_id   = id;
                reg.patch<lux::ecs::MeshComponent>(h.sphere,
                    [&](lux::ecs::MeshComponent& mc) { mc.material_asset_id = id; });
                h.swap_in_flight = true;
                h.swap_frames    = 0;
            }
        }

        // 5. 参数直呼(最后一次赢)。只对自己注册的临时材质发 —— 首刀就绪前球
        //    穿的是共享的 PreviewGrey,对它 modify 会波及缩略图等所有灰球。
        if (h.params_dirty && h.params_valid)
            if (const auto* gpu = reg.try_get<lux::ecs::MeshGpuCacheComponent>(h.sphere);
                gpu && !gpu->material.isNull()
                    && h.owned_temp_ids.contains(gpu->material_source))
            {
                const auto submitted = lux::render::modifyGraphMaterial(
                    lux::render::MaterialUploadClient(
                        infra_.upload,
                        h.material_ops
                    ),
                    gpu->material,
                    h.params
                );
                if (submitted)
                    h.params_dirty = false;
            }
    }

    // ── Teardown ─────────────────────────────────────────────────────────────

    bool MaterialPreviewHost::releaseGpu()
    {
        if (!host_) return true;
        Host& h = *host_;
        if (h.runtime)
        {
            // tearDown 里驻留胶水 releaseRefs 会让临时材质归零,GPU
            // 副本(材质 + 着色器模块)由归零广播的驻留编排回收;
            // CPU shell 由 shutdown 收。
            if (infra_.close_driver == nullptr)
            {
                lux::log::error(
                    "material-preview",
                    "preview has no MainCloseDriver"
                );
                return false;
            }
            const auto report = infra_.close_driver->close(*h.runtime);
            if (!report)
            {
                lux::log::error(
                    "material-preview",
                    "preview scene close timed out");
                return false;
            }
            h.runtime.reset();
        }
        if (h.target)
        {
            // Ours to destroy (we created it). SAMPLED 池经统一退休入口回收。
            if (auto closing = h.target.close(); closing)
            {
                (void)infra_.control->syncCall(
                    std::move(closing.value())
                );
            }
        }
        ready_ = false;
        return true;
    }

    void MaterialPreviewHost::shutdown()
    {
        // CPU 侧清账(渲染线程死活无关):临时材质 shell + 归零订阅 + 待换内容。
        if (!host_) return;
        Host& h = *host_;
        h.subs.clear();   // 退订先行:removeAsset 会触发广播,别再回喂自己
        for (const auto& id : h.owned_temp_ids)
            assets_.removeAsset(id);
        h.owned_temp_ids.clear();
        h.retire_on_unref.clear();
        h.pending_unref.clear();
        h.pending_payload.reset();
        ready_ = false;
    }

} // namespace lux::editor
