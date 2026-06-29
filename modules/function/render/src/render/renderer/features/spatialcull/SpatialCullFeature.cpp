#include <lux/engine/render/renderer/features/spatialcull/SpatialCullFeature.hpp>
#include <lux/engine/render/renderer/features/spatialcull/SpatialCullOperation.hpp>

#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/scene/SpatialCullGrid.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>   // FeatureFactory
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>   // typed-op Param registration
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <cstring>
#include <utility>

namespace lux::render
{
    SpatialCullFeature::SpatialCullFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{std::move(cfg.name)})
        , params_{cfg.cell_size, cfg.cull_distance}
    {}

    void SpatialCullFeature::initAndAttachTo(RenderScene& sc)
    {
        // Feature owns its scene resource (PointCloud/Trajectory pattern): emplace
        // the SpatialCullGrid here, NOT in the general RenderScene constructor.
        auto& reg = sc.sceneRegistry();
        if (!reg.find<SpatialCullGrid>())
            reg.emplace<SpatialCullGrid>();

        auto* grid = reg.find<SpatialCullGrid>();
        auto& ctx  = renderContext();
        SpatialCullGrid::InitInfo gi{};
        gi.device_context   = &ctx.deviceContext();
        gi.deferred_queue   = &ctx.deferredDestroyQueue();
        gi.frames_in_flight = ctx.framesInFlight();
        gi.cell_size        = params_.cell_size;
        gi.cull_distance    = params_.cull_distance;
        grid->init(gi);   // idempotent
    }

    void SpatialCullFeature::onDetachFromScene(RenderScene& /*sc*/)
    {
        // SpatialCullGrid lifetime is owned by the scene registry (torn down at
        // scene teardown alongside the other per-scene resources); nothing to do.
    }

    void SpatialCullFeature::addPasses(RGBuilder& /*builder*/)
    {
        // No render-graph passes: the grid is a host-mapped GPU buffer the mesh
        // cull reads via buffer-device-address (the address is published into the
        // scene's instance-cull-mask primitive in onFrameBegin).
    }

    void SpatialCullFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
    {
        auto& reg  = renderScene().sceneRegistry();
        auto* grid = reg.find<SpatialCullGrid>();
        auto* inst = reg.find<InstanceResources>();
        auto* cam  = reg.find<ViewCameraResource>();
        if (grid == nullptr || inst == nullptr)
            return;

        // Cull sources = the active views' world camera positions. Pull them
        // through the scene's general view accessor (no domain knowledge here).
        camera_scratch_.clear();
        renderScene().forEachActiveView([this, cam](View& v)
        {
            const auto* cam_fd = cam ? cam->find(v.handle.index) : nullptr;
            if (!cam_fd)
                return;
            const auto& p = cam_fd->camera_transform.position;
            camera_scratch_.push_back({p.x(), p.y(), p.z()});
        });

        // Classify cells + upload the per-slot mask for THIS frame, then publish its
        // GPU address into the scene's domain-neutral primitive. The mesh cull reads
        // that address (0 = no mask) — it never names SpatialCullGrid.
        grid->update(renderScene().frameSerial(), camera_scratch_, *inst);
        renderScene().setInstanceCullMaskAddress(grid->activeMaskAddress());
    }

    RenderFeature::EParamApply SpatialCullFeature::applyParams(const void* src, std::size_t size)
    {
        if (src == nullptr || size != sizeof(SpatialCullParams))
            return EParamApply::UNSUPPORTED;
        std::memcpy(&params_, src, sizeof(SpatialCullParams));
        // Push to the live grid; it re-classifies cells on the next onFrameBegin
        // with the new edge length / distance — no graph rebuild, so HOT.
        if (auto* grid = renderScene().sceneRegistry().find<SpatialCullGrid>())
        {
            grid->setCellSize(params_.cell_size);
            grid->setCullDistance(params_.cull_distance);
        }
        return EParamApply::HOT;
    }

    // ── Factory (grid pattern: feature added via addFeatureFactory + a FIP) ──
    static FeatureHandle spatialCullCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        SpatialCullCommConfig cc{};
        if (param != nullptr && param_size >= sizeof(SpatialCullCommConfig))
            cc = *static_cast<const SpatialCullCommConfig*>(param);

        SpatialCullFeature::Config cfg{};
        cfg.cell_size     = cc.cell_size;
        cfg.cull_distance = cc.cull_distance;
        return sc->addFeature<SpatialCullFeature>(cfg);
    }

    // SpatialCull exposes live params (paramStructName/applyParams) → it needs the generic
    // setParams op so the settings panel's Apply actually reaches it. One Param op; the
    // registrar registers it via the shared registerFeatureParamsOp and derives
    // param_set_op_index from its position. (Previously makeSimpleFactory registered NO op
    // and left param_set_op_index=-1, so Apply silently no-op'd — a real gap, now closed.)
    struct SpatialCullParamsOp
    {
        using Payload = SetFeatureParamsPayload;
        static constexpr EOpKind kind = EOpKind::Param;
        static constexpr const char* name = "SpatialCullParams";
    };
    using SpatialCullOps = FeatureOpRegistrar<ServerOp<SpatialCullParamsOp>>;

    const FeatureFactory kSpatialCullFeatureFactory{
        &spatialCullCreateFn,
        &SpatialCullOps::registerAll,
        &SpatialCullOps::unregisterAll,
        "SpatialCull",
        SpatialCullOps::kParamSetOpIndex,
    };

} // namespace lux::render
