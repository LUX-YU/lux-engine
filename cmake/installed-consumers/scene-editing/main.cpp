#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/SceneDocument.hpp>
#include <lux/engine/editor/ScenePackageFile.hpp>
#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>

#include <cassert>
#include <chrono>

using namespace lux;

template <class T> auto take(T result)
{
    assert(result);
    return std::move(*result);
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    static_cast<void>(object::ObjectRuntime::instance());
    auto engine = take(engine::EngineContext::create({1, 64, {64}}, 64));
    execution::TaskScope tasks{engine->execution()};
    const auto schemas =
        take(simulation::ecs::ComponentSchemaSet::build(simulation::ecs::transformComponentSchemas(), {}));
    const auto loading = scene::worldLoadingSystemRegistration();
    std::vector<std::byte> configuration;
    const scene::WorldLoadingConfiguration bootstrap{{partition::PartitionOrdinal{0}}};
    assert(loading.configuration.encode(&bootstrap, configuration));
    scene::SceneDescriptionBuilder description;
    assert(description.addSystem(
        {1},
        "loading",
        loading.type,
        loading.description->version,
        loading.description->configuration_schema_name,
        loading.description->configuration_schema_version,
        configuration
    ));
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("c850a52d-4638-41b8-a052-2cb35c0a174d")};
    const std::array author_schemas{world::worldDataSchemaId("lux.ecs.Transform3D")};
    auto package = take(scene::createScenePackage(
        asset::AssetId{ids("scene")},
        "Installed authoring",
        author_schemas,
        std::make_shared<const simulation::SimulationDescription>(),
        take(std::move(description).buildResolved())
    ));
    auto world = std::shared_ptr<const world::WorldDescription>{package.world, &package.world->data()};
    auto source = take(process::world_loading::makeWorldMemoryStorageSource(world, package.volumes));
    scene::WorldLoadingServices loading_services{source, tasks};
    loading_services.initial_partitions = package.partitions;
    auto provider = scene::makeSceneCapabilityProvider<scene::WorldLoadingServices>(
        "loading",
        "lux.world.loading",
        loading_services
    );
    const simulation::SimulationSystemRegistry systems;
    auto& runtime = engine->sceneRuntime();
    auto lease = take(runtime.builder()
                          .setDescription({package.scene, &package.scene->data()})
                          .setWorld(world)
                          .setSimulation({package.simulation, &package.simulation->data()})
                          .setRegistrations(schemas, systems, std::span(&loading, 1))
                          .setProviders(std::span(&provider, 1))
                          .setFrameAdmissionRequired(false)
                          .build());
    assert(runtime.pauseSimulation(lease.id()));
    auto document = std::make_unique<editor::SceneDocument>(runtime, lease.id(), schemas, package);
    const world::WorldObjectId entity{ids("entity")};
    const auto& schema = *schemas.find(cxx::typeToken<simulation::ecs::Transform3D>());
    simulation::ecs::WorldEntityMap references;
    simulation::ecs::Transform3D transform;
    auto capture = schema.capture_value(&transform, schema.code_lifetime);
    editor::SceneComponentData component{schema.id, schema.version, take(capture.encode(references, 4096))};
    editor::SceneCreateObjects create{{{entity, {0}, {std::move(component)}}}};
    assert(document->apply({0, std::move(create)}));
    transform.translation.x() = 3;
    auto edit = take(document->prepareComponentEdit(entity, document->revision(), transform));
    assert(document->apply(edit) && document->undo() && document->redo());
    assert(!document->apply(edit)); // Its original revision cannot be reused after Undo/Redo.
    const auto revision = document->revision();
    auto saved = take(document->capture());
    auto bytes = take(scene::encodeScenePackage(saved, 1U << 20U));
    assert(editor::writeScenePackageBytesAtomic(argv[1], bytes, editor::ESceneWrite::REPLACE));
    assert(document->saved(saved, revision) && !document->dirty());
    auto reopened = take(editor::readScenePackageFile(argv[1], 1U << 20U));
    assert(reopened.scene->id() == package.scene->id());
    assert(reopened.partitions.front()->objectAt(0).id() == entity);
    auto decoded = take(document->component(entity, schema.id));
    assert(static_cast<const simulation::ecs::Transform3D*>(decoded.data())->translation.x() == 3);
    const auto retired = lease.retire();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!retired.complete())
    {
        assert(std::chrono::steady_clock::now() < deadline);
        assert(engine->execution().dispatchMainCompletions());
        assert(runtime.driveFrame(scene::ESceneDriveMode::MAINTENANCE));
    }
    document.reset();
    assert(runtime.instanceCount() == 0);
}
