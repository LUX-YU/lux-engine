#pragma once
/**
 * @file LightFeature.hpp
 * @brief Scene-light data owner (LightResources storage + light CRUD ops).
 *
 * This is the DECOUPLED home of the light DOMAIN. The render core (RenderScene)
 * no longer emplaces LightResources in its constructor; this feature does, in
 * initAndAttachTo (the PointCloud/Trajectory feature-owned-resource pattern).
 * The light create/update/destroy/batch commands are feature-scoped dynamic ops
 * (see LightOperation.hpp); the core protocol no longer names light.
 *
 * OWNERSHIP vs CONSUMPTION: LightFeature owns the light DATA. DeferredLighting /
 * Forward / Shadow features are CONSUMERS (find<LightResources>); they must be
 * ordered AFTER LightFeature (ShadowResources caches a raw LightResources* at its
 * own attach), and tolerate its absence (no LightFeature = unlit scene).
 *
 * A scene that doesn't add this feature pays nothing (no per-scene light SSBO)
 * and renders unlit. Distinct from DeferredLightingFeature, which is the lighting
 * COMPUTE pass — this owns the light data it consumes.
 */

#include <lux/engine/render/SceneFeature.hpp>
#include <lux/engine/function/visibility.h>

#include <string>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC LightFeature final : public SceneFeature
    {
    public:
        struct Config
        {
            std::string name{"Light"};
        };

        LightFeature();
        explicit LightFeature(Config cfg);

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onFrameBegin(const FeatureFrameContext& context) override;
        void onDetachFromScene(RenderScene& scene) override;
        [[nodiscard]] bool canRebaseSceneOrigin(
            const std::int64_t origin_delta[3]) const noexcept override;
        void rebaseSceneOrigin(
            const std::int64_t origin_delta[3]) noexcept override;
    };

    // No-arg ctor defined out-of-class so Config{} is evaluated where the class is
    // complete (GCC 11/12 reject Config{} / {} as an in-class default argument).
    inline LightFeature::LightFeature() : LightFeature(Config{}) {}

} // namespace lux::render
