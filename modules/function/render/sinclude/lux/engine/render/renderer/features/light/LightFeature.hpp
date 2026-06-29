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

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/visibility.h>

#include <string>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC LightFeature final : public RenderFeature
    {
    public:
        struct Config
        {
            std::string name{"Light"};
        };

        explicit LightFeature(Config cfg = {});

        void initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;   ///< no RG passes
    };

} // namespace lux::render
