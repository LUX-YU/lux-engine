#pragma once
/**
 * @file StandardMeshStackFeature.hpp
 * @brief Owner of the per-scene standard 3D mesh-stack resources.
 *
 * Decouples the 3D mesh foundation out of the RenderScene core constructor. The core
 * used to emplace InstanceResources / VertexPoolRegistry / StaticVertexPoolSet /
 * VertexProductionRegistry unconditionally — so a 2D / headless / compute-only
 * scene paid for ~96MB vertex/index arenas + an instance SSBO + the Set-7
 * bindless vertex pool it never used. Now those resources are FEATURE-owned
 * (the LightFeature / SpatialCullFeature pattern): adding this feature IS the
 * opt-in to standard mesh rendering; omitting it costs nothing.
 *
 * Resource owner only — no render-graph passes. Must attach BEFORE the
 * mesh-consuming features (Skinning / ForwardMesh / DeferredGBuffer / MeshShadow
 * / Highlight cache InstanceResources / read VertexPoolRegistry at their own
 * attach), so the editor/scenes register it as the first mesh-related feature.
 *
 * Stage A: resource ownership only — the mesh / upload ops still ride the core
 * protocol; they move to feature-scoped typed-ops in the op-downloading stage.
 *
 * See .internal/feature-classification-and-2d-coupling-2026-06-21.md §1D.
 */

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/visibility.h>

#include <string>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC StandardMeshStackFeature final : public RenderFeature
    {
    public:
        struct Config
        {
            std::string name{"StandardMeshStack"};
        };

        StandardMeshStackFeature();
        explicit StandardMeshStackFeature(Config cfg);

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onFrameBegin(const FeatureFrameContext& context) override;
        void onDetachFromScene(RenderScene& scene) override;
        [[nodiscard]] bool canRebaseSceneOrigin(const std::int64_t origin_delta[3]) const noexcept override;
        void rebaseSceneOrigin(const std::int64_t origin_delta[3]) noexcept override;
    };

    // No-arg ctor defined out-of-class so Config{} is evaluated where the class is
    // complete (GCC 11/12 reject Config{} / {} as an in-class default argument).
    inline StandardMeshStackFeature::StandardMeshStackFeature() : StandardMeshStackFeature(Config{})
    {
    }

} // namespace lux::render
