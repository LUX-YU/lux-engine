#pragma once

#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/mesh/RasterMeshParameters.hpp>

namespace lux::render::detail
{
    // Immutable material/geometry preparation. These functions do not inspect Storage, Graph,
    // resource identities or a View and can execute on the shared CPU scheduler.
    [[nodiscard]] RenderResult<rdesc::ShaderProgramArtifact>
    buildMeshShader(const rdesc::MaterialProgramSet&, rdesc::EMaterialPurpose, bool paged, bool reflected) noexcept;

    [[nodiscard]] RenderResult<GraphicsState> meshGraphicsState(
        std::uint32_t vertex_stride,
        std::span<const MeshAttribute> attributes,
        const rdesc::MaterialProgramSet&,
        const rdesc::ShaderInfo&,
        bool front_counter_clockwise,
        bool reflected
    ) noexcept;

    // Shared geometric ports describe data, not material groups or pipeline counts.
    void describeMeshDrawInputs(FeaturePorts&);
    [[nodiscard]] RasterWork meshRasterStage(FeatureBuildContext&, bool camera_view = true);
    [[nodiscard]] RenderResult<void> bindMeshDraws(
        FeatureBuildContext&,
        GraphPassId,
        rdesc::EMaterialPurpose,
        GraphResourceId views = {},
        GraphResourceId raster_states = {},
        bool front_counter_clockwise = true
    ) noexcept;

    template <bool Depth> RenderResult<void> describe(const void*, std::string_view, FeaturePorts&) noexcept;

    template <bool Depth>
    RenderResult<void>
    build(
        const void*,
        std::string_view,
        FeatureBuildContext&
    ) noexcept;
} // namespace lux::render::detail
