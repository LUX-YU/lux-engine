#pragma once
/**
 * @file StandardMaterialFeature.hpp
 * @brief Owner of the global MaterialResources stack.
 *
 * Decouples the shading-model / material machinery out of RenderServer::init. The
 * core used to emplace MaterialResources unconditionally —
 * so a 2D / unlit / headless server paid for the 5 family SSBOs + variant buckets +
 * the built-in shading-model registry it never used, and the core NAMED the shading
 * models (packMaterialType / EShadingModel). Now those are FEATURE-owned (the
 * LightFeature / StandardMeshStack pattern): adding this feature IS the opt-in to
 * standard materials; omitting it costs nothing, and a custom shading model is added
 * by extending the material/lighting features — the core never changes.
 *
 * Resource owner only — no render-graph passes. MUST attach BEFORE every material
 * consumer (DeferredGBuffer / ForwardMesh / Highlight read MaterialResources +
 * bind MaterialResources at set 4) AND before StandardMeshStack (addMeshInstance
 * reads the material slot), so the editor/scenes register it as the first
 * material-related feature.
 *
 * Stage D-2: resource ownership only — the material upload ops still ride the core
 * protocol; they move to feature-scoped typed-ops in the op-downloading stage (D-3).
 */

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/visibility.h>

#include <string>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC StandardMaterialFeature final : public RenderFeature
    {
    public:
        struct Config
        {
            std::string name{"StandardMaterial"};
        };

        StandardMaterialFeature();
        explicit StandardMaterialFeature(Config cfg);

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
    };

    // No-arg ctor defined out-of-class so Config{} is evaluated where the class is
    // complete (GCC 11/12 reject Config{} / {} as an in-class default argument).
    inline StandardMaterialFeature::StandardMaterialFeature() : StandardMaterialFeature(Config{})
    {
    }

} // namespace lux::render
