#pragma once
#include <lux/cxx/container/SmallVector.hpp>
#include <lux/engine/context/render/RenderAssetPipeline.hpp>
#include <lux/engine/description/Visual.hpp>
#include <lux/engine/render/inputs/RasterInstanceInputs.hpp>

namespace lux::scene::detail
{
    // One adopted representation. The role is a cold Plan slot, never a GPU identity or serialized author value.
    struct MeshRenderResource final
    {
        std::uint32_t role;
        engine::render::RenderAssetVersion mesh_source, material_source;
        render::MeshId mesh;
        render::MaterialId material;
        render::MeshInstanceId instance;
        rdesc::MeshVisualDescription visual;
    };

    struct MeshRenderResourceComponent final
    {
        // The usual single-role entity allocates no separate array. Different roles remain independently owned.
        cxx::SmallVector<MeshRenderResource, 1> roles;

        [[nodiscard]] const MeshRenderResource* find(std::uint32_t role) const noexcept
        {
            const auto entry = std::ranges::find(roles, role, &MeshRenderResource::role);
            return entry == roles.end() ? nullptr : &*entry;
        }
    };
} // namespace lux::scene::detail
