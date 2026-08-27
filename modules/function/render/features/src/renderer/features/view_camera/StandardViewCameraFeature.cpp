#include <lux/engine/render/renderer/features/view_camera/StandardViewCameraFeature.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>

#include <lux/engine/render/scene/RenderScene.hpp>

#include <utility>

namespace lux::render
{
    StandardViewCameraFeature::StandardViewCameraFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{std::move(cfg.name)})
    {
    }

    lux::render::Expected<void> StandardViewCameraFeature::initAndAttachTo(RenderScene& sc)
    {
        // Own the per-scene 3D camera store. ensure<T>: idempotent get-or-create
        // (whoever attaches first builds it). No GPU state — it is a plain CPU
        // mirror filled by the camera op handler each frame and read by the camera
        // consumers (cull / shadow / hzb / deferred lighting).
        sc.sceneRegistry().ensure<ViewCameraResource>();
        return {};
    }

    void StandardViewCameraFeature::onDetachFromScene(RenderScene& /*sc*/)
    {
        // Resource owned by the scene registry; torn down at scene teardown after
        // the camera-consuming features that read it (this feature registers first
        // → tears down last). Nothing to do here.
    }

    void StandardViewCameraFeature::deallocateViewState(uint32_t view)
    {
        // A view was destroyed (RenderScene::removeView → deallocateViewState per
        // feature). Evict its camera entry so a later REUSED view id can't inherit
        // stale matrices / frustum — the camera used to die with View::cached_frame_data.
        if (auto* cam = sceneView().sceneRegistry().find<ViewCameraResource>())
            cam->removeView(view);
    }

    bool StandardViewCameraFeature::canRebaseSceneOrigin(const std::int64_t origin_delta[3]) const noexcept
    {
        const auto* cameras = renderScene().sceneRegistry().find<ViewCameraResource>();
        return cameras == nullptr || cameras->canRebaseSceneOrigin(origin_delta);
    }

    void StandardViewCameraFeature::rebaseSceneOrigin(const std::int64_t origin_delta[3]) noexcept
    {
        if (auto* cameras = renderScene().sceneRegistry().find<ViewCameraResource>())
        {
            cameras->rebaseSceneOrigin(origin_delta);
        }
    }

    // (原先这里有一个空的 addPasses:本单元只拥有资源,不产 render-graph pass。
    //  现在它继承 RenderFeature,不再被迫实现 addPasses。)

    // The factory (kViewCameraFeatureFactory) + its createFn + the camera
    // update op live in ViewCameraOperationHandlers.cpp, next to the handler their
    // register_ops_fn binds (the grid / light layout).

} // namespace lux::render
