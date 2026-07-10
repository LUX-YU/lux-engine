/**
 * @file Scene3D.cpp
 * @brief The 3D kit's install hooks — the one TU that pulls the EcsRenderTraits
 *        specializations + render_bridge's registerComponent machinery, keeping that
 *        template weight out of every consumer.
 */

#include <lux/pack/d3/Scene3D.hpp>

#include <memory>

#include <lux/engine/ecs/World.hpp>
#include <lux/pack/d3/world/systems/TransformSystem.hpp>
#include <lux/pack/d3/world/systems/CameraSystem.hpp>
#include <lux/pack/d3/world/systems/AnimationSystem.hpp>

#include <lux/engine/render_bridge/RenderableSystem.hpp>
#include <lux/engine/render_bridge/RegisterComponent.hpp>   // RenderableSystem::registerComponent<C> definition
#include <lux/pack/d3/render_bridge/MeshRenderTraits.hpp>    // EcsRenderTraits<Mesh/SkeletalMesh>
#include <lux/pack/d3/render_bridge/SkyboxRenderTraits.hpp>  // EcsRenderTraits<Skybox>
#include <lux/pack/d3/render_bridge/GridRenderTraits.hpp>    // EcsRenderTraits<Grid>
#include <lux/pack/d3/render_bridge/LightRenderTraits.hpp>   // EcsRenderTraits<Directional/Point/Spot>

namespace lux::pack
{
    using lux::ecs::World;

    void installSystems(lux::ecs::World& world)
    {
        // Canonical order, matching World's retired hardcoded ctor: local TRS →
        // world matrix (Transform), then view/proj from world transform (Camera),
        // then clip sampling + skinning matrices (Animation).
        world.addSystem(std::make_unique<TransformSystem>());
        world.addSystem(std::make_unique<CameraSystem>());
        world.addSystem(std::make_unique<AnimationSystem>());
    }

    void registerRenderables(lux::render_bridge::RenderableSystem& rs)
    {
        rs.registerComponent<MeshComponent>();           // INSTANCE (static)
        rs.registerComponent<SkeletalMeshComponent>();   // INSTANCE (skinned)
        rs.registerComponent<SkyboxComponent>();         // PARAM
        rs.registerComponent<GridComponent>();           // PARAM
        rs.registerComponent<DirectionalLightComponent>(); // POOL
        rs.registerComponent<PointLightComponent>();       // POOL
        rs.registerComponent<SpotLightComponent>();        // POOL
    }

} // namespace lux::pack
