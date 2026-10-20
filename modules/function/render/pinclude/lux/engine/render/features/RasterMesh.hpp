#pragma once

#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/FeatureDefinition.hpp>
#include <lux/engine/render/features/MaterialMeshRaster.hpp>
#include <lux/engine/render/features/MeshDepthRaster.hpp>

namespace lux::render::detail
{
    template <bool Depth> RenderResult<void> describe(const void*, std::string_view, ResourceClaims&) noexcept;

    template <bool Depth>
    RenderResult<void> build(
        const void* value,
        std::string_view instance,
        std::span<const ResolvedResource> resources,
        RenderGraphBuilder& builder
    ) noexcept;

    template <bool Depth, bool Paged>
    RenderResult<ProgramContribution>
    programs(const void*, std::string_view, const RenderGraphDefinition&, const EnabledDeviceFeatures&) noexcept;
} // namespace lux::render::detail
