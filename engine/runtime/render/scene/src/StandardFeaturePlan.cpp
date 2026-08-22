#include <lux/engine/runtime/render/scene/StandardFeaturePlan.hpp>

#include <lux/engine/render/comm/server/RenderServer.hpp>

#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/HzbOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredLightingOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LineListOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TonemapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LinearDepthOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/FogOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/HighlightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/StreamingFeedbackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SpatialCullOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ForwardMeshOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkyboxOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkinningOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Grid2DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/RenderClusterOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TerrainOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/WaterOperation.ops.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>

namespace lux::runtime
{
    const RenderProfile& standardDesktopProfile() noexcept
    {
        /// 「装了这个 profile 就有延迟管线」。此前这份清单是 3D 包的
        /// runtime pack 的 ECS contribution 不再携带 render pass 根。
        /// 顺序不在这里管:attach 先后由目录的 resolveAttachOrder 给出。
        static constexpr std::string_view kPassRoots[] = {
            "ShadowMap",          // 阴影图集
            "MeshShadow",         // 网格投影 pass
            "DeferredGBuffer",    // 延迟管线
            "DeferredLighting",
            "Fog",
            "Water",
            "Tonemap",
            "LinearDepth",        // sensors / SSAO / DoF 的公共输入
            "RenderCluster",      // World static-instance Cluster Registry
            "Terrain",            // paged Heightfield cache and patch renderer
            "Hzb"};               // 遮挡剔除层级 z
        static constexpr RenderProfile kProfile{ /*name=*/{}, kPassRoots };
        return kProfile;
    }

    const RenderProfile& previewProfile() noexcept
    {
        /// 前向小场景:阴影双 pass + 前向网格,无延迟栈/后处理。相机/灯光/网格栈/
        /// 材质等 ECS 提取节点驱动的 feature 不在这里 ——
        /// `RenderStage::requiredFeatures` 由 RenderSystemStages 取并集。
        /// name = "preview" 让 ShadowMap 的小图集配置行胜出。
        static constexpr std::string_view kPassRoots[] = {
            "ShadowMap",     // 预览小阴影图集(profile="preview" 的那行配置)
            "MeshShadow",    // 网格投影 pass
            "ForwardMesh"};  // 前向网格 pass(唯一的着色 pass)
        static constexpr RenderProfile kProfile{ /*name=*/"preview", kPassRoots };
        return kProfile;
    }

    namespace
    {
        /// CommConfig → 精确 sizeof 的字节。static_assert 与服务端
        /// decodeCommConfig 的精确长度匹配是同一条契约的两端。
        template <class C>
        [[nodiscard]] std::vector<std::byte> toBytes(const C& cfg)
        {
            static_assert(std::is_trivially_copyable_v<C>,
                "CommConfig 必须可平凡拷贝 —— 它是按字节进 attach 目录的。");
            const auto* p = reinterpret_cast<const std::byte*>(&cfg);
            return std::vector<std::byte>(p, p + sizeof(C));
        }
    } // namespace

    // ── The ONE home for the standard feature plan + configs ─────
    // 例外:modules/function/render/test/scene_cycle_stress_test.cpp 为门禁
    // 独立性手抄了同一份 3D 配置组装(它声明 mirror 1:1)——改这里的配置时
    // 同步检查那边,否则压测覆盖面静默漂移。
    void registerStandardRenderFeatures(lux::render::GeneralRenderServer&        server,
                                        lux::render::FeatureCatalog&             registry,
                                        std::vector<lux::render::FeatureAttach>& plan_out)
    {
        // ── 1. Feature init configs —— 按值进纯数据条目(toBytes),无悬垂。
        //       The order matches attach-time resource dependencies:
        //       shadow infra → geometry → post → grid.
        lux::render::ShadowMapCommConfig    shmap_cfg{};
        shmap_cfg.enable_directional_csm = 1u;
        // Spot/point shadow casters are culled past this camera distance to bound
        // the shared shadow atlas. The 60 m default is far too small for authored
        // scenes (lights routinely sit hundreds of units from the camera), which
        // silently dropped EVERY non-directional shadow — only the sun (no
        // distance cull) ever cast. 0 = no limit; Top-K + atlas budget still bound
        // how many casters actually get slices. Tunable live via ShadowQualityParams.
        shmap_cfg.non_directional_shadow_max_distance = 0.0f;

        lux::render::MeshShadowCommConfig   mshsw_cfg{};
        mshsw_cfg.comm_config_version       = lux::render::kMeshShadowCommConfigVersion;
        mshsw_cfg.descriptor_layout_version = lux::render::kMeshShadowDescriptorLayoutVersion;

        lux::render::DeferredGBufferCommConfig gbuf_cfg{};
        gbuf_cfg.comm_config_version       = lux::render::kDeferredGBufferCommConfigVersion;
        gbuf_cfg.descriptor_layout_version = lux::render::kDeferredGBufferDescriptorLayoutVersion;
        // HZB occlusion-culling toggle: view-cull reads the previous frame's
        // max-Z pyramid to cull occluded instances.
        gbuf_cfg.extension_flags          |= lux::render::EGpuDrivenMeshExt::HZB;

        // cluster 参数走 comm 默认;只显式写偏离默认的项。
        lux::render::DeferredLightingCommConfig lit_cfg{};
        lit_cfg.enable_clustered = 1;

        // ── chooseGBufferReadMode:上层查设备能力后显式二选一 ─────────────
        //
        // 这是 plan 构建期**唯一**的运行期决策(其余条目是纯静态数据)——所以
        // 它是一个具名步骤,而不是散在配置块里的分支(ADR 裁决四)。
        // 引擎提供两条并行能力,装哪条是产品侧的事;要 local-read 而设备给
        // 不了,装配直接报 err::lighting::LocalReadUnsupported,没有静默降级。
        // 两个 CommConfig 在同一个 lambda 里一起写,所以不可能只改一边;引擎在
        // attach 期还会交叉校验一次(err::lighting::ReadModeScopeMismatch)。
        const auto chooseGBufferReadMode =
            [&](lux::render::DeferredGBufferCommConfig&  gbuf,
                lux::render::DeferredLightingCommConfig& lit)
        {
            const auto caps = server.deviceCaps();
            if (caps && caps->dynamic_rendering_local_read)
            {
                // 路径 B:G-buffer 与光照并进同一个 local-read 作用域,光照用
                // subpassLoad() 读 tile 本地数据 —— 省掉一整轮 G-buffer 读写带宽。
                gbuf.extension_flags |= lux::render::EGpuDrivenMeshExt::LocalReadScope;
                lit.read_mode         = lux::render::ELightingReadMode::INPUT_ATTACHMENT;
            }
            else
            {
                // 路径 A:两个独立 scope,光照 texture() 采样 G-buffer。视觉等价。
                lit.read_mode         = lux::render::ELightingReadMode::SAMPLED;
            }
        };
        chooseGBufferReadMode(gbuf_cfg, lit_cfg);

        lux::render::SkyboxCommConfig       sky_cfg{};
        // tone_map_op 默认即 ACES;exposure/gamma 亦为默认值,显式写出仅为
        // 上层可见的调参入口。
        lux::render::TonemapCommConfig      tm_cfg{
            .tone_map_op = lux::render::ETonemapOperator::ACES_FILMIC,
            .exposure    = 1.0f,
            .gamma       = 2.2f,
        };
        lux::render::Grid3DCommConfig         grid_cfg{};
        // Line-list feature config left default — built-in shaders +
        // 200k max vertices (plenty for selection AABB / debug lines / gizmos).
        lux::render::LineListTransientCommConfig line_cfg{};
        lux::render::HighlightCommConfig         hl_cfg{};    // shaders default to builtins
        lux::render::StreamingFeedbackCommConfig streaming_cfg{};
        // SkinningFeature has no per-instance config — its only inputs are the
        // bone palettes uploaded via the feature-scoped SkinningProxy.

        // ── 2. Register feature TYPES in ATTACH order + build the per-scene
        //       attach plan. Light FIRST (owns LightResources that
        //       shadow/gbuffer/forward/deferred consume — ShadowMap caches a raw
        //       LightResources* at attach), Skinning next (produces the post-skin
        //       vertex buffer the mesh draws sample). Everything after is
        //       RG-ordered by data dependency, so list position past those two is
        //       moot — and lookups are BY NAME, so adding/reordering needs no
        //       index bookkeeping. Canvas2D costs zero draws while its instance
        //       pool is empty; the minimal per-kind feature profile is driven by
        //       the explicit render profile and LXSC contributions.
        // 这是一份**有序目录**，不是「装什么」的决定（阶段 3：kinds 位掩码删除，
        // 理由见 FeatureAttach.hpp）。谁被 attach 由子系统的声明并集 + 宿主自己
        // 声明的纯 pass 决定；这里只负责两件事：
        //   ① 注册 feature TYPE（进程域，必须在渲染服务端线程上做）；
        //   ② 定下 attach 的**顺序** —— Material 先于 MeshStack、MeshStack 先于
        //      Skinning、ViewCamera 先于一切相机消费者、Grid2D 后于 Canvas2D
        //      (同 stage 的写后写靠注册序分先后，后者在上)。集合表达不了顺序，
        //      所以这份目录留着。
        // Absent-feature consumers no-op gracefully (invalid ops/handle — the
        // documented contract).
        lux::render::FeatureCatalog              reg;
        std::vector<lux::render::FeatureAttach>  plan;
        const auto add = [&](const lux::render::FeatureFactory& factory, const auto& cfg)
        {
            // 注册动作留在拿着服务端引用的这一层;目录只收回执 —— 目录头因此
            // 不再 include 整个 RenderServer.hpp(公共头减重,惠及全部下游)。
            const auto reply = server.addFeatureFactory(factory);
            reg.add(factory, reply.feature_type_id,
                    std::span<const lux::render::TypeId>(reply.ops, reply.op_count));
            plan.push_back(lux::render::FeatureAttach{
                factory.name ? factory.name : "", /*profile=*/"",
                reply.feature_type_id, toBytes(cfg) });
        };
        // Plan-only entry: a SECOND profile of an already-registered feature
        // TYPE (different config per kind — e.g. the preview's small shadow
        // atlas). No reg.add: the TYPE registration is process-domain and
        // already done by the paired add() above it.
        const auto addProfile = [&](const char* name, const char* profile, const auto& cfg)
        {
            plan.push_back(lux::render::FeatureAttach{
                name, profile, reg.typeId(name), toBytes(cfg) });
        };

        add(lux::render::kViewCameraFeatureFactory, lux::render::ViewCameraCommTag{});   // per-view camera state — BEFORE every camera consumer; 2D binding model uploads through it too
        add(lux::render::kLightFeatureFactory,      lux::render::LightCommTag{});
        add(lux::render::kMaterialFeatureFactory,   lux::render::MaterialCommTag{});  // MaterialResources — BEFORE StandardMeshStack
        add(lux::render::kMeshStackFeatureFactory,  lux::render::MeshStackCommTag{});  // mesh-stack resources — BEFORE Skinning + mesh consumers
        add(lux::render::kSkinningFeatureFactory,   lux::render::SkinningCommConfig{});
        add(lux::render::kShadowMapFeatureFactory,          shmap_cfg);
        // Preview profile of ShadowMap: the thumbnail/material preview's SMALL
        // atlas — same TYPE, its own config. Placed right after the standard
        // entry so the preview attach order keeps shadow → mesh-shadow → forward.
        lux::render::ShadowMapCommConfig shmap_preview_cfg{};
        shmap_preview_cfg.atlas_page_resolution = 2048;
        shmap_preview_cfg.atlas_page_count      = 2;
        shmap_preview_cfg.max_shadow_slices     = 64;
        addProfile(lux::render::kShadowMapFeatureFactory.name, "preview", shmap_preview_cfg);
        add(lux::render::kMeshShadowFeatureFactory,         mshsw_cfg);
        // ForwardMesh — the preview SceneRuntimes (the editor's ThumbnailService
        // + MaterialPreviewHost, both on previewProfile()) render forward-only
        // (no deferred stack); no standard scene attaches it, so the TYPE rides
        // in the plan as preview-exclusive. Config lives HERE only, eliminating
        // the duplicate write that used to exist elsewhere.
        lux::render::ForwardMeshCommConfig fwd_cfg{};
        fwd_cfg.comm_config_version       = lux::render::kForwardMeshCommConfigVersion;
        fwd_cfg.descriptor_layout_version = lux::render::kForwardMeshDescriptorLayoutVersion;
        add(lux::render::kForwardMeshFeatureFactory,        fwd_cfg);
        add(lux::render::kDeferredGBufferFeatureFactory,    gbuf_cfg);
        add(lux::render::kDeferredLightingFeatureFactory,   lit_cfg);
        add(lux::render::kSkyboxFeatureFactory,             sky_cfg);
        add(lux::render::kFogFeatureFactory,                lux::render::FogCommConfig{});
        add(lux::render::kWaterFeatureFactory,              lux::render::WaterCommConfig{});
        add(lux::render::kTonemapFeatureFactory,            tm_cfg);
        // R2-3:LinearDepth 生产者 —— 装上即声明"补 LinearDepth 槽 + 读
        // SceneDepth",物理背衬/usage/图导入由槽声明管道自动补齐;
        // 产出是 sensors / SSAO / DoF 的公共输入(R32F 全屏解算,代价可忽略)。
        lux::render::LinearDepthCommConfig ld_cfg{};
        add(lux::render::kLinearDepthFeatureFactory,        ld_cfg);
        add(lux::render::kGrid3DFeatureFactory,               grid_cfg);
        add(lux::render::kLineListFeatureFactory,         line_cfg);     // debug/gizmo lines — any host, both kinds
        add(lux::render::kHzbFeatureFactory,                lux::render::HzbCommTag{});
        add(lux::render::kHighlightFeatureFactory,          hl_cfg);  // mesh-instance-flag based — no 2D consumer
        add(lux::render::kStreamingFeedbackFeatureFactory,  streaming_cfg);
        // 这两个是**有字段**的 CommConfig(cell_size/cull_distance、offscreen_groups),
        // 要的就是它们各自的默认值 —— 显式送出默认构造值,而不是靠"长度不够就当
        // 默认"蒙混:后者让"配置没送到"和"就要默认值"在线上无法区分。
        add(lux::render::kSpatialCullFeatureFactory,        lux::render::SpatialCullCommConfig{});
        add(lux::render::kRenderClusterFeatureFactory,      lux::render::RenderClusterCommTag{});
        lux::render::TerrainCommConfig terrain_cfg{};
#if defined(__ANDROID__)
        // Mobile keeps the exact same cooked Terrain identity and hierarchy;
        // only the device work/residency budget is lower.
        terrain_cfg.page_capacity = 64u;
        terrain_cfg.maximum_selected_patches = 256u;
#endif
        add(lux::render::kTerrainFeatureFactory,            terrain_cfg);
        add(lux::render::kCanvas2DFeatureFactory,           lux::render::Canvas2DCommConfig{});  // 2D content-viewport rendering (image/tilemap/pixel field)
        // Grid2D AFTER Canvas2D on purpose: its "Over" pass shares the Overlay
        // stage with the Canvas2D content pass, and the write-after-write
        // tie-break orders same-stage passes by registration — after = on top.
        // (The "Under" pass rides the Transparent stage and needs no ordering.)
        lux::render::Grid2DCommConfig grid2d_cfg{};
        add(lux::render::kGrid2DFeatureFactory,             grid2d_cfg);  // 2D reference grid (renders only when a grid entity exists)

        registry = std::move(reg);
        plan_out = std::move(plan);
    }

} // namespace lux::runtime
