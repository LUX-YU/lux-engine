#pragma once
// ============================================================================
//  ViewCameraResource.hpp — per-scene store of per-view 3D camera state.
//
//  Owned by StandardViewCameraFeature (the LightFeature / SpatialCullFeature
//  resource-ownership pattern). Holds the per-view camera data (view/proj
//  matrices + derived inverses, world camera position, frustum, viewport) that
//  the core View used to carry inline. Consumers — Forward/Deferred cull
//  (GpuDrivenMeshFeatureBase), MeshShadow, Hzb, PointCloud, SpatialCull,
//  DeferredLighting — read it via sceneRegistry().find<ViewCameraResource>().
//
//  A 2D / headless / compute-only scene that omits StandardViewCamera carries
//  none of this — the core View stays domain-neutral (extent + GPU slot only).
// ============================================================================

#include <lux/engine/render/scene/ViewFrameData.hpp>   // ViewFrameData (CameraView, Frustum, ...)
#include <lux/engine/render/scene/ViewStateTable.hpp>

#include <cstdint>

namespace lux::render
{
    class ViewCameraResource
    {
    public:
        /// Upsert a view's camera state (called by the StandardViewCamera op handler).
        void setView(uint32_t view_id, const ViewFrameData& data) { views_.set(view_id, data); }

        /// Drop a view's camera state (view removed / scene teardown).
        void removeView(uint32_t view_id) noexcept { (void)views_.erase(view_id); }

        /// Per-view camera state, or nullptr if this view has none yet.
        [[nodiscard]] const ViewFrameData* find(uint32_t view_id) const noexcept { return views_.find(view_id); }

        [[nodiscard]] bool empty() const noexcept { return views_.empty(); }

    private:
        ViewStateTable<ViewFrameData> views_;
    };

} // namespace lux::render
