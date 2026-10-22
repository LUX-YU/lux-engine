#pragma once

#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

namespace lux::render::detail
{
    [[nodiscard]] RenderResult<void> describeMeshGroup(
        const RasterMeshGroup&,
        std::string_view prefix,
        rdesc::EMaterialPurpose,
        FeaturePorts&,
        std::uint32_t visible_capacity = 0,
        std::uint32_t view_count = 1
    ) noexcept;

    void bindVisibleMeshDraw(
        RasterWork&,
        FeatureBuildContext&,
        std::string_view prefix,
        std::uint32_t capacity,
        std::uint32_t view,
        std::uint32_t winding
    ) noexcept;

    [[nodiscard]] RenderResult<RasterWork> buildMeshGroup(
        const RasterMeshGroup&,
        std::string_view prefix,
        rdesc::EMaterialPurpose,
        bool paged,
        FeatureBuildContext&,
        bool derive_members = true,
        bool reflected = false
    ) noexcept;

    template <bool Depth> RenderResult<void> describe(const void*, std::string_view, FeaturePorts&) noexcept;

    template <bool Depth, bool Paged>
    RenderResult<void> build(const void*, std::string_view, FeatureBuildContext&) noexcept;
} // namespace lux::render::detail
