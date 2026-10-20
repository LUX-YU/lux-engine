#pragma once

#include <lux/engine/render/features/FeatureDefinition.hpp>
#include <lux/engine/render/features/MaterialMeshRaster.hpp>
#include <lux/engine/render/features/MeshDepthRaster.hpp>

namespace lux::render::detail
{
    template <bool Depth> RenderResult<void> describe(const void*, std::string_view, FeaturePorts&) noexcept;

    template <bool Depth, bool Paged>
    RenderResult<void> build(const void*, std::string_view, FeatureBuildContext&) noexcept;
} // namespace lux::render::detail
