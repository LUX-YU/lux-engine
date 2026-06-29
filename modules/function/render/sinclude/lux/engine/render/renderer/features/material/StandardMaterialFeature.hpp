#pragma once
/**
 * @file StandardMaterialFeature.hpp
 * @brief Owner of the global material stack (ShadingModelRegistry + MaterialResources).
 *
 * Decouples the shading-model / material machinery out of RenderServer::init. The
 * core used to emplace ShadingModelRegistry + MaterialResources unconditionally —
 * so a 2D / unlit / headless server paid for the 5 family SSBOs + variant buckets +
 * the built-in shading-model registry it never used, and the core NAMED the shading
 * models (packMaterialType / EShadingModel). Now those are FEATURE-owned (the
 * LightFeature / StandardMeshStack pattern): adding this feature IS the opt-in to
 * standard materials; omitting it costs nothing, and a custom shading model is added
 * by extending the material/lighting features — the core never changes.
 *
 * Resource owner only — no render-graph passes. MUST attach BEFORE every material
 * consumer (DeferredGBuffer / ForwardMesh / Highlight read ShadingModelRegistry +
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

        explicit StandardMaterialFeature(Config cfg = {});

        void initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;   ///< no RG passes (resource owner)
    };

} // namespace lux::render
