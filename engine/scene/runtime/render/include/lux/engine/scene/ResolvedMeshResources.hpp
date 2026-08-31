#pragma once

#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>

namespace lux::scene
{
    struct ResolvedMeshResources final
    {
        asset::AssetId mesh_source{};
        asset::AssetId material_source{};
        render::RMeshHandle mesh{};
        render::RMaterialHandle material{};

        friend bool operator==(const ResolvedMeshResources&, const ResolvedMeshResources&) noexcept = default;
    };
} // namespace lux::scene
