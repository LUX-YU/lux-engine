#pragma once
#include <lux/engine/editor/EditorSelection.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>

namespace lux::editor::workbench::detail
{
    inline bool acceptsResource(EditorSceneHandle requested_scene, simulation::ecs::Entity requested_entity,
        asset::AssetId requested_mesh, asset::AssetId requested_material, std::uint64_t requested_serial,
        EditorSceneHandle current_scene, simulation::ecs::Entity current_entity,
        asset::AssetId current_mesh, asset::AssetId current_material, std::uint64_t current_serial) noexcept
    {
        return requested_scene.valid() && requested_scene == current_scene && requested_entity == current_entity &&
            requested_mesh == current_mesh && requested_material == current_material &&
            requested_serial != 0 && requested_serial == current_serial;
    }
}
