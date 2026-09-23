#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>

static void measure(std::size_t count)
{
    using namespace lux;
    using namespace simulation::ecs;
    using Clock = std::chrono::steady_clock;
    const std::array positions{Eigen::Vector3f{-1, -1, 0}, Eigen::Vector3f{1, -1, 0}, Eigen::Vector3f{0, 1, 0}};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    auto geometry = scene::MeshQueryGeometry::build(positions, indices);
    assert(geometry);
    const asset::AssetId mesh{std::array<std::uint8_t, 16>{1}};
    Registry registry;
    scene::MeshQuerySystem query{registry};
    query.setGeometry(mesh, *geometry);
    std::vector<Entity> entities;
    for (std::size_t index{}; index < count; ++index)
    {
        const auto entity = registry.create();
        registry.emplace<Mesh3D>(entity).value.mesh = mesh;
        registry.emplace<WorldTransform3D>(entity).value.translation() =
            Eigen::Vector3d{double(index % 100) * 4, double(index / 100) * 4, 10};
        entities.push_back(entity);
    }
    const auto begin = Clock::now();
    query.updateStablePoint();
    const auto built = Clock::now();
    const auto before = query.statistics();
    scene::RayHit3D hit;
    std::uint64_t checksum{};
    std::size_t nodes{}, candidates{};
    for (std::size_t index{}; index < 100; ++index)
    {
        const auto item = index % count;
        const math::Ray3d ray{{double(item % 100) * 4, double(item / 100) * 4, 0}, Eigen::Vector3d::UnitZ()};
        assert(query.raycastNearest(ray, 100, hit).value());
    }
    const auto started = Clock::now();
    for (std::size_t index{}; index < 10000; ++index)
    {
        const auto item = index % count;
        const math::Ray3d ray{{double(item % 100) * 4, double(item / 100) * 4, 0}, Eigen::Vector3d::UnitZ()};
        scene::MeshQueryWork work;
        const auto found = query.raycastNearest(ray, 100, hit, &work);
        assert(found && *found && hit.entity == entities[item] && hit.distance == 10);
        checksum += entityBits(hit.entity) + static_cast<std::uint64_t>(hit.distance);
        nodes += work.tree_nodes;
        candidates += work.candidates;
    }
    const auto queried = Clock::now();
    registry.patch<WorldTransform3D>(entities.front(), [](auto &value) { value.value.translation().z() = 11; });
    query.updateStablePoint();
    const auto updated = Clock::now();
    const auto after = query.statistics();
    assert(before.rebuilds == after.rebuilds && after.leaf_updates == before.leaf_updates + 1);
    assert(after.geometries == 1 && candidates == 10000 && nodes < 10000 * 50);
    const auto us = [](auto value) { return std::chrono::duration<double, std::micro>(value).count(); };
    std::printf("MEASURE entities=%zu queries=10000 warmup=100 tree_nodes=%zu candidates=%zu checksum=%llu "
                "build_us=%.3f query_us=%.3f one_refit_us=%.3f shared_bvhs=%zu index_accounted_bytes=%zu "
                "shared_geometry_bytes=%zu rebuilds=%llu leaf_updates=%llu\n",
                count, nodes, candidates, static_cast<unsigned long long>(checksum), us(built - begin),
                us(queried - started), us(updated - queried), after.geometries, after.retained_bytes,
                after.geometry_bytes, static_cast<unsigned long long>(after.rebuilds),
                static_cast<unsigned long long>(after.leaf_updates));
}

static void checkDriverInvalidation()
{
    using namespace lux;
    namespace ecs = simulation::ecs;
    const auto registration = scene::builtinMeshQuerySystemRegistration();
    const lux::simulation::ecs::ComponentSchemaSet task_components{};
    const lux::simulation::SimulationSystemRegistry task_system_types;
    const std::array task_scene_systems{registration};
    scene::SceneDescriptionBuilder builder;
    assert(builder.addSystem({1}, "query", registration.type, 1, {}, 0));
    auto description = std::move(builder).buildResolved();
    assert(description);
    auto made = scene::SceneInstance::create({std::make_shared<const scene::SceneDescription>(std::move(*description)),
                                              std::make_shared<const world::WorldDescription>(),
                                              std::make_shared<const simulation::SimulationDescription>(),
                                              task_components, task_system_types, task_scene_systems,
                                              {},
                                              simulation::ESimulationMode::DERIVATION});
    assert(made && (*made)->simulation().seal());
    auto instance = std::move(*made);
    auto executor = task::TaskExecutor::create({0, 1024});
    assert(executor);
    scene::SceneDriver driver(*executor);
    const auto advance = [&] {
        scene::SceneAdvanceBudget budget;
        assert(driver.advance(*instance, std::chrono::steady_clock::now(), budget) == scene::ESceneProgress::COMPLETE);
        assert(instance->progress().result);
    };
    advance();
    auto &query = *instance->findSceneSystem<scene::MeshQuerySystem>();
    const std::array positions{Eigen::Vector3f{-1, -1, 0}, Eigen::Vector3f{1, -1, 0}, Eigen::Vector3f{0, 1, 0}};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    auto geometry = scene::MeshQueryGeometry::build(positions, indices);
    assert(geometry);
    const asset::AssetId mesh{std::array<std::uint8_t, 16>{1}};
    const auto entity = instance->registry().create();
    instance->registry().emplace<ecs::Mesh3D>(entity).value.mesh = mesh;
    instance->registry().emplace<ecs::WorldTransform3D>(entity).value.translation().z() = 5;
    query.setGeometry(mesh, *geometry);
    scene::RayHit3D hit;
    const math::Ray3d ray{{0, 0, 0}, Eigen::Vector3d::UnitZ()};
    assert(!query.raycastNearest(ray, 100, hit));
    advance(); // No external invalidate(), Renderer, or direct stable-point call.
    assert(query.raycastNearest(ray, 100, hit).value() && hit.entity == entity && hit.distance == 5);
    instance->registry().patch<ecs::WorldTransform3D>(entity, [](auto &value) { value.value.translation().z() = 7; });
    advance();
    assert(query.raycastNearest(ray, 100, hit).value() && hit.distance == 7);
    const auto refreshes = instance->progress().refresh_completed;
    advance();
    assert(instance->progress().refresh_completed == refreshes);
    std::puts(
        "PASS paused Driver: query component/geometry invalidation reaches stable point; idle turn does not rebuild");
}

int main(int argc, char **argv)
{
    if (argc == 2)
    {
        const auto count = std::strtoul(argv[1], nullptr, 10);
        assert(count == 1000 || count == 10000);
        measure(count);
        return 0;
    }
    checkDriverInvalidation();
    using namespace lux;
    using namespace simulation::ecs;
    const std::array positions{Eigen::Vector3f{-1, -1, 0}, Eigen::Vector3f{1, -1, 0}, Eigen::Vector3f{0, 1, 0}};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    auto geometry = scene::MeshQueryGeometry::build(positions, indices);
    assert(geometry);
    const asset::AssetId mesh{std::array<std::uint8_t, 16>{1}};
    Registry registry;
    scene::MeshQuerySystem query{registry};
    const auto create = [&](double z) {
        const auto entity = registry.create();
        auto &visual = registry.emplace<Mesh3D>(entity);
        visual.value.mesh = mesh;
        visual.value.visible = true;
        auto &transform = registry.emplace<WorldTransform3D>(entity);
        transform.value.translation() = Eigen::Vector3d{1.0e12, 0, z};
        return entity;
    };
    const auto distant = create(10.0);
    const auto nearest = create(2.0);
    math::Ray3d ray{Eigen::Vector3d{1.0e12, 0, 0}, Eigen::Vector3d::UnitZ()};
    scene::RayHit3D hit;
    query.updateStablePoint();
    const auto unavailable = query.raycastNearest(ray, 100, hit);
    assert(!unavailable && unavailable.error().code == scene::EMeshQueryError::NOT_READY);

    query.setGeometry(mesh, *geometry);
    query.updateStablePoint();
    scene::MeshQueryWork work;
    auto found = query.raycastNearest(ray, 100, hit, &work);
    assert(found && *found && hit.entity == nearest && std::abs(hit.distance - 2.0) < 1.0e-6);
    const auto rebuilt = query.statistics().rebuilds;

    registry.patch<WorldTransform3D>(nearest, [](auto &transform) { transform.value.translation().z() = 20.0; });
    assert(!query.raycastNearest(ray, 100, hit));
    query.updateStablePoint();
    found = query.raycastNearest(ray, 100, hit);
    assert(found && *found && hit.entity == distant && query.statistics().rebuilds == rebuilt);

    registry.destroy(distant);
    const auto replacement = create(30.0);
    assert(replacement != distant);
    query.updateStablePoint();
    found = query.raycastNearest(ray, 100, hit);
    assert(found && *found && hit.entity == nearest);

    // Singular poses are a query failure, not a miss; repairing one must retry.
    registry.patch<WorldTransform3D>(nearest, [](auto &transform) { transform.value.linear().setZero(); });
    query.updateStablePoint();
    const auto invalid = query.raycastNearest(ray, 100, hit);
    assert(!invalid && invalid.error().code == scene::EMeshQueryError::INVALID_TRANSFORM);
    registry.patch<WorldTransform3D>(nearest, [](auto &transform) { transform.value.linear().setIdentity(); });
    query.updateStablePoint();
    found = query.raycastNearest(ray, 100, hit);
    assert(found && *found);

    const auto before = query.statistics();
    for (int index = 0; index < 100; ++index)
    {
        found = query.raycastNearest(ray, 100, hit);
        assert(found && *found);
    }
    assert(query.statistics().rebuilds == before.rebuilds);
    assert(query.statistics().leaf_updates == before.leaf_updates);
    assert(query.statistics().geometries == 1);

    ray.origin.y() = 5.0;
    const auto old_hit = hit.entity;
    found = query.raycastNearest(ray, 100, hit);
    assert(found && !*found && hit.entity == old_hit);
    query.removeGeometry(mesh);
    query.updateStablePoint();
    assert(!query.raycastNearest(ray, 100, hit));
    std::puts("PASS mesh query: nearest, readiness, refit, large origin, generations, repair, shared geometry, stable "
              "queries");
}
