/**
 * @file Scene3D.cpp
 * @brief The 3D kit's install hooks — the one TU that pulls the EcsRenderTraits
 *        specializations + render_bridge's registerComponent machinery, keeping that
 *        template weight out of every consumer.
 */

#include <lux/engine/gameplay/3d/Scene3D.hpp>

#include <memory>

#include <lux/engine/gameplay/world/World.hpp>
#include <lux/engine/gameplay/3d/world/systems/TransformSystem.hpp>
#include <lux/engine/gameplay/3d/world/systems/CameraSystem.hpp>
#include <lux/engine/gameplay/3d/world/systems/AnimationSystem.hpp>

#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>
#include <lux/engine/gameplay/render_bridge/RegisterComponent.hpp>   // RenderableSystem::registerComponent<C> definition
#include <lux/engine/gameplay/3d/traits/MeshRenderTraits.hpp>    // EcsRenderTraits<Mesh/SkeletalMesh>
#include <lux/engine/gameplay/3d/traits/SkyboxRenderTraits.hpp>  // EcsRenderTraits<Skybox>
#include <lux/engine/gameplay/3d/traits/GridRenderTraits.hpp>    // EcsRenderTraits<Grid>
#include <lux/engine/gameplay/3d/traits/LightRenderTraits.hpp>   // EcsRenderTraits<Directional/Point/Spot>

namespace lux::gameplay::d3
{
    void installSystems(lux::gameplay::World& world)
    {
        // Canonical order, matching World's retired hardcoded ctor: local TRS →
        // world matrix (Transform), then view/proj from world transform (Camera),
        // then clip sampling + skinning matrices (Animation).
        world.addSystem(std::make_unique<TransformSystem>());
        world.addSystem(std::make_unique<CameraSystem>());
        world.addSystem(std::make_unique<AnimationSystem>());
    }

    void registerRenderables(lux::gameplay::RenderableSystem& rs)
    {
        rs.registerComponent<MeshComponent>();           // INSTANCE (static)
        rs.registerComponent<SkeletalMeshComponent>();   // INSTANCE (skinned)
        rs.registerComponent<SkyboxComponent>();         // PARAM
        rs.registerComponent<GridComponent>();           // PARAM
        rs.registerComponent<DirectionalLightComponent>(); // POOL
        rs.registerComponent<PointLightComponent>();       // POOL
        rs.registerComponent<SpotLightComponent>();        // POOL
    }

} // namespace lux::gameplay::d3
