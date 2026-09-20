#include <cassert>
#include <cstdio>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

int main()
{
    using namespace lux;
    using namespace simulation::ecs;
    Registry world;
    scene::MeshQuerySystem query(world);
    const std::array positions{Eigen::Vector3f{-1, -1, 0}, Eigen::Vector3f{1, -1, 0}, Eigen::Vector3f{0, 1, 0}};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    const auto geometry = scene::MeshQueryGeometry::build(positions, indices);
    assert(geometry);
    const asset::AssetId source{std::array<std::uint8_t, 16>{1}};
    query.setGeometry(source, *geometry);
    const auto entity = world.create();
    world.emplace<Mesh3D>(entity).value.mesh = source;
    world.emplace<WorldTransform3D>(entity).value.translation().z() = 4;
    query.updateStablePoint();
    scene::RayHit3D hit;
    const auto found = query.raycastNearest({Eigen::Vector3d::Zero(), Eigen::Vector3d::UnitZ()}, 10, hit);
    assert(found && *found && hit.entity == entity && hit.distance == 4);
    assert(query.statistics().geometries == 1);
    std::puts("PASS installed MeshQuery: actual Entity, no Camera/RenderSystem/Renderer/backend");
}
