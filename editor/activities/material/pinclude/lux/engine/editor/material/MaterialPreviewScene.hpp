#pragma once
#include <lux/engine/editor/material/MaterialPreview.hpp>
#include <lux/engine/world/WorldDescription.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/description/Visual.hpp>

namespace lux::editor::material
{
    struct MaterialPreviewScene final
    {
        std::shared_ptr<const world::WorldDescription> world;
        std::shared_ptr<const lux::scene::SceneDescription> scene;
        std::shared_ptr<const simulation::SimulationDescription> simulation;
        cxx::SharedBytes<> world_volume;
        simulation::ecs::Transform3D camera_pose, light_pose;
        lux::scene::Camera camera;
        rdesc::LightDescription light;
    };
    [[nodiscard]] MaterialPreviewResult<MaterialPreviewScene>
    makeMaterialPreviewScene(std::span<const render::RenderFeatureRegistration>);
}
