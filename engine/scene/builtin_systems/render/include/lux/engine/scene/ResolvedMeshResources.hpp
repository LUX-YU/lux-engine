#pragma once

#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/function/render/features/resources/ResourceHandles.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <memory>

namespace lux::scene
{
struct ResolvedMeshResources final
{
    asset::AssetId mesh_source{};
    asset::AssetId material_source{};
    render::RMeshHandle mesh{};
    render::RMaterialHandle material{};
    // CPU extraction and accepted Programs retain this use independently
    // of the Registry. It contains no pointer back into a Scene instance.
    std::shared_ptr<const void> lifetime;

    friend bool operator==(const ResolvedMeshResources &, const ResolvedMeshResources &) noexcept = default;
};
} // namespace lux::scene
