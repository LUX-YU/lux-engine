#include <lux/engine/editor/scene/systems/StreamingSceneSystem.hpp>

#include <lux/engine/editor/app/LuxEditor.hpp>   // EditorRenderInfra (feature_registry)

#include <lux/engine/gameplay/world/World.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>
#include <lux/engine/gameplay/3d/world/components/SceneSettingsComponent.hpp>

#include <lux/engine/asset/AssetManager.hpp>

#include <lux/engine/ui/UIRenderSession.hpp>                  // SceneTickContext::session (RenderSession base)
#include <lux/engine/render/comm/client/RenderSession.hpp>    // FeatureParamsProxy upcast target
#include <lux/engine/render/renderer/features/spatialcull/SpatialCullParams.hpp>
#include <lux/engine/render/renderer/features/FeatureParamsOperation.hpp>  // FeatureParamsProxy

#include <array>
#include <span>
#include <utility>

namespace lux::editor
{
    // -------------------------------------------------------------------------
    StreamingSceneSystem::StreamingSceneSystem(
        std::function<void(const lux::asset::asset_id_t&)> load_sink)
    {
        // W2b INC-2:把渲染桥同款异步加载回调交给流送系统,使它对预取带
        // (prefetch_range 内、仍 dormant)的 cell 实体提前后台载数据,cell 激活时
        // 数据已就位 → 消 pop-in。复用同一 request_load_ 注入 seam,无新接口。
        streaming_.setLoadSink(std::move(load_sink));
    }

    // -------------------------------------------------------------------------
    void StreamingSceneSystem::onPreRenderableUpdate(const SceneTickContext& ctx)
    {
        auto& reg = ctx.world.registry();

        // Scene-global view + streaming settings (SceneSettingsComponent — the UE
        // WorldSettings model). Source: SceneSettingsComponent if present, else the
        // WorldStreamingSystem's own defaults. The two halves stay decoupled
        // (UE-style): streaming params feed the gameplay
        // WorldStreamingSystem; cull_distance is MIRRORED to the render SpatialCull
        // feature. Applied ONLY on change (dirty): the cull mirror is a comm push,
        // too costly to send every frame.
        lux::gameplay::d3::WorldStreamingSystem::Params sp{};   // system defaults
        bool  have_cull    = false;
        float cull_distance = 0.f;

        if (auto v = reg.view<lux::gameplay::d3::SceneSettingsComponent>(); v.begin() != v.end())
        {
            const auto& s = v.get<lux::gameplay::d3::SceneSettingsComponent>(*v.begin());
            sp.enabled          = s.enabled;
            sp.cell_size        = s.cell_size;
            sp.load_range       = s.load_range;
            sp.unload_range     = s.unload_range;
            sp.prefetch_range   = s.prefetch_range;
            sp.evict_age_frames = s.evict_age_frames;
            have_cull           = true;
            cull_distance       = s.cull_distance;
        }
        // No SceneSettingsComponent → WorldStreamingSystem keeps its own defaults.

        const bool changed =
            !applied_ ||
            sp.enabled          != last_enabled_  ||
            sp.cell_size        != last_cell_     ||
            sp.load_range       != last_load_     ||
            sp.unload_range     != last_unload_   ||
            sp.prefetch_range   != last_prefetch_ ||
            sp.evict_age_frames != last_evict_    ||
            (have_cull && cull_distance != last_cull_distance_);
        if (changed)
        {
            streaming_.setParams(sp);
            // Mirror cull_distance → render SpatialCull (handle/op by name).
            if (have_cull)
            {
                const auto sc_op = ctx.infra.feature_registry.paramSetOp("SpatialCull");
                if (sc_op != lux::render::kInvalidTypeId)
                {
                    lux::render::SpatialCullParams scp{};   // cell_size = SpatialCull default
                    scp.cull_distance = cull_distance;
                    lux::render::FeatureParamsProxy(ctx.session).setParams(
                        ctx.scene_id, ctx.infra.feature_registry.handle("SpatialCull"),
                        sc_op, &scp, sizeof(scp));
                }
            }
            last_enabled_       = sp.enabled;
            last_cell_          = sp.cell_size;
            last_load_          = sp.load_range;
            last_unload_        = sp.unload_range;
            last_prefetch_      = sp.prefetch_range;
            last_evict_         = sp.evict_age_frames;
            last_cull_distance_ = cull_distance;
            applied_            = true;
        }

        // 大世界① 增量2 W2a:用相机世界位作流送源,在桥接到 GPU 之前按 cell 距离给
        // 远处实体打 RenderDormantComponent。必须夹在 world_->tick(WorldTransform 已
        // 就绪)与 renderable_system_->update(drive 排除 dormant + reap 拆实例)之间。
        const std::array<float, 3> src{ctx.eye.x(), ctx.eye.y(), ctx.eye.z()};
        streaming_.update(reg, std::span<const std::array<float, 3>>(&src, 1));
    }

    // -------------------------------------------------------------------------
    void StreamingSceneSystem::onPostRenderableUpdate(const SceneTickContext& ctx)
    {
        // W2b INC-3(W2c):休眠超龄 cell 的 CPU 资产数据驱逐 —— 内存随活跃 cell 稳定,
        // 不随世界总量涨。必须在 renderable_system_->update 之后:其 reap 已把休眠实体
        // 的 GPU 实例拆掉(refcount→0 → 桥缓存条目擦除),此时"桥无该 id 条目"== 全局
        // 无引用 == 可安全释放 CPU 数据。共享 mesh/material 若仍被活跃 cell 引用,会被
        // isAssetReferenced 挡住;再唤醒时 requestLoad 自动重解码(往返机制不变)。
        if (!ctx.assets)
            return;

        for (const auto& id : streaming_.evictableAssets())
            if (!ctx.renderable.isAssetReferenced(id))
                (void)ctx.assets->unloadData(id);
    }

} // namespace lux::editor
