#include <lux/engine/render/renderer/features/spatialcull/SpatialCullFeature.hpp>
#include <lux/engine/function/render/client/genops/SpatialCullOperation.ops.hpp>

#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/scene/SpatialCullGrid.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/mesh/MeshCullCandidateSource.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>

#include <cstring>
#include <utility>

namespace lux::render
{
    SpatialCullFeature::SpatialCullFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{std::move(cfg.name)}), params_{cfg.cell_size, cfg.cull_distance}
    {
    }

    lux::render::Expected<void> SpatialCullFeature::initAndAttachTo(RenderScene& sc)
    {
        // Feature owns its scene resource (PointCloud/Trajectory pattern): emplace
        // the SpatialCullGrid here, NOT in the general RenderScene constructor.
        // ensure<>:本 feature 自建自有,句柄存成员,onFrameBegin 不再每帧重查。
        auto& reg = sc.sceneRegistry();
        auto& ctx = renderContext();

        SpatialCullGrid::InitInfo gi{};
        gi.device_context = &ctx.deviceContext();
        gi.deferred_queue = &ctx.deferredDestroyQueue();
        gi.frames_in_flight = ctx.framesInFlight();
        gi.cell_size = params_.cell_size;
        gi.cull_distance = params_.cull_distance;

        // ensure<T>(init_args) — the registry constructs + inits and publishes
        // only on success, so grid_ is never a half-built instance.
        auto grid = reg.ensure<SpatialCullGrid>(gi);
        if (!grid)
            return lux::cxx::unexpected<RenderError>(grid.error());
        grid_ = *grid;
        return {};
    }

    void SpatialCullFeature::onDetachFromScene(RenderScene& /*sc*/)
    {
        // SpatialCullGrid lifetime is owned by the scene registry (torn down at
        // scene teardown alongside the other per-scene resources); nothing to do.
    }

    // (原先这里有一个空的 addPasses:本单元不产 render-graph pass —— 网格是一块
    //  host-mapped 的 GPU 缓冲,网格剔除经 buffer-device-address 读它,地址在
    //  onFrameBegin 里发布到场景的实例剔除掩码原语上。现在它继承 RenderFeature,
    //  不再被迫实现 addPasses。)

    void SpatialCullFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
    {
        // grid_ 是自建的,attach 后恒非空。inst / cam 记忆化:两者都归别的 feature
        // 所有且本 feature 未声明 requires=,缺失是合法运行态,所以判空保留、
        // 只把每帧的哈希查找收敛成一次。
        auto& reg = renderScene().sceneRegistry();
        // RenderCluster's candidate source is the authoritative coarse filter.
        // Do not rebuild the legacy per-slot distance mask in that mode: besides
        // intersecting two unrelated distance policies, doing so scans every alive
        // instance once per frame and turns an otherwise window-bounded world back
        // into O(total resident slots).  A zero address means "no secondary mask".
        if (const auto* candidates = reg.find<MeshCullCandidateSource>(); candidates != nullptr && candidates->active())
        {
            renderScene().setInstanceCullMaskAddress(0ull);
            return;
        }
        if (inst_cache_ == nullptr)
            inst_cache_ = reg.find<InstanceResources>();
        auto* inst = inst_cache_;
        auto* cam = resolveViewCameraOnce(cam_cache_, reg);
        if (inst == nullptr)
            return;

        // Cull sources = the active views' world camera positions. Pull them
        // through the scene's general view accessor (no domain knowledge here).
        camera_scratch_.clear();
        renderScene().forEachActiveView([this, cam](View& v) {
            const auto* cam_fd = cam ? cam->find(v.handle.index) : nullptr;
            if (!cam_fd)
                return;
            const auto& p = cam_fd->camera_transform.position;
            camera_scratch_.push_back({p.x(), p.y(), p.z()});
        }
        );

        // Classify cells + upload the per-slot mask for THIS frame, then publish its
        // GPU address into the scene's domain-neutral primitive. The mesh cull reads
        // that address (0 = no mask) — it never names SpatialCullGrid.
        grid_->update(renderScene().frameSerial(), camera_scratch_, *inst);
        renderScene().setInstanceCullMaskAddress(grid_->activeMaskAddress());
    }

    RenderFeature::EParamApply SpatialCullFeature::applyParams(const void* src, std::size_t size)
    {
        if (src == nullptr || size != sizeof(SpatialCullParams))
            return EParamApply::UNSUPPORTED;
        std::memcpy(&params_, src, sizeof(SpatialCullParams));
        // Push to the live grid; it re-classifies cells on the next onFrameBegin
        // with the new edge length / distance — no graph rebuild, so HOT.
        // grid_ 由 attach 期建立。applyParams 走 comm 命令面,理论上可在 attach 前
        // 到达(参数命令与 AddFeature 无序保证),故此处判空是真守卫。
        if (grid_ != nullptr)
        {
            grid_->setCellSize(params_.cell_size);
            grid_->setCullDistance(params_.cull_distance);
        }
        return EParamApply::HOT;
    }

} // namespace lux::render
