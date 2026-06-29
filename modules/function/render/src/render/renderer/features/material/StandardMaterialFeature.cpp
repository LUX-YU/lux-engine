#include <lux/engine/render/renderer/features/material/StandardMaterialFeature.hpp>
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp>

#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/RendererContext.hpp>

#include <utility>

namespace lux::render
{
    // Exported by RenderServer.cpp: lazily builds the GLOBAL material stack
    // (ShadingModelRegistry + MaterialResources, registry-first). Forward-declared
    // to avoid pulling the server-impl headers into a feature.
    void ensureGlobalMaterialResources(RenderContext& ctx);

    StandardMaterialFeature::StandardMaterialFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{std::move(cfg.name)})
    {}

    void StandardMaterialFeature::initAndAttachTo(RenderScene& /*sc*/)
    {
        // Own the global material stack (registry-first; MaterialResources::init
        // requires ShadingModelRegistry). Idempotent + shared across scenes (lives
        // in the global registry). Adding this feature IS the opt-in to the stack —
        // moved out of RenderServer::init so a 2D / unlit / headless server pays
        // nothing.
        ensureGlobalMaterialResources(renderContext());
    }

    void StandardMaterialFeature::onDetachFromScene(RenderScene& /*sc*/)
    {
        // Global resource — never torn down per-scene (shared across scenes);
        // destroyed at server shutdown with the global registry. Nothing to do.
    }

    void StandardMaterialFeature::addPasses(RGBuilder& /*builder*/)
    {
        // Resource owner only — no render-graph passes. The material set-4 binding +
        // per-family pipelines live in the mesh-draw consumers (DeferredGBuffer /
        // ForwardMesh / Highlight), which read the resources this feature owns.
    }

    // The factory (kStandardMaterialFeatureFactory) + its createFn + the 3
    // feature-scoped material ops live in MaterialOperationHandlers.cpp, next to
    // the handlers their register_ops_fn binds (the grid / light layout).

} // namespace lux::render
