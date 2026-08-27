#pragma once
/**
 * @file StandardViewCameraFeature.hpp
 * @brief Owner of the per-scene 3D camera state (ViewCameraResource) + the
 *        per-view camera update op.
 *
 * Decouples the 3D camera (view/proj matrices, world camera position, frustum)
 * out of the core View / RenderScene. The core View used to carry the camera
 * matrices + frustum inline and broadcast them to every feature; now they are
 * FEATURE-owned (the LightFeature / SpatialCullFeature pattern): adding this
 * feature IS the opt-in to 3D camera data; omitting it leaves the core View
 * domain-neutral (extent + an opaque per-view GPU slot only).
 *
 * Resource owner only — no render-graph passes. Must attach BEFORE every camera
 * consumer (Forward / Deferred / MeshShadow / Hzb / SpatialCull / PointCloud /
 * DeferredLighting), which read ViewCameraResource — owner-first ordering, like
 * StandardMeshStack.
 *
 * See .internal/feature-classification-and-2d-coupling-2026-06-21.md (decoupling View from 3D).
 */

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/visibility.h>

#include <string>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC StandardViewCameraFeature final : public RenderFeature
    {
    public:
        struct Config
        {
            std::string name{"StandardViewCamera"};
        };

        StandardViewCameraFeature();
        explicit StandardViewCameraFeature(Config cfg);

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        void deallocateViewState(uint32_t view) override; ///< evict the destroyed view's camera entry
        [[nodiscard]] bool canRebaseSceneOrigin(const std::int64_t origin_delta[3]) const noexcept override;
        void rebaseSceneOrigin(const std::int64_t origin_delta[3]) noexcept override;
    };

    // No-arg ctor defined out-of-class so Config{} is evaluated where the class is
    // complete (GCC 11/12 reject Config{} / {} as an in-class default argument).
    inline StandardViewCameraFeature::StandardViewCameraFeature() : StandardViewCameraFeature(Config{})
    {
    }

} // namespace lux::render
