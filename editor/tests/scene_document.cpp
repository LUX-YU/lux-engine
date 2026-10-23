#include "support/ContextFixture.hpp"
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/SceneDocument.hpp>
#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>

using namespace lux;
using namespace lux::editor;
namespace ecs = simulation::ecs;

namespace
{
    template <class T> auto take(T result)
    {
        assert(result);
        return std::move(*result);
    }

    template <class T>
    SceneComponentData encode(
        const ecs::ComponentSchemaSet& schemas,
        const ecs::WorldEntityMap& identities,
        const T& value
    )
    {
        const auto& schema = *schemas.find(cxx::typeToken<T>());
        auto captured = schema.capture_value(&value, schema.code_lifetime);
        return {schema.id, schema.version, take(captured.encode(identities, 4096))};
    }

    struct Updates final
    {
        unsigned replaced{}, created{}, removed{};

        void replace(ecs::Registry&, ecs::Entity) noexcept
        {
            ++replaced;
        }

        void create(ecs::Registry&, ecs::Entity) noexcept
        {
            ++created;
        }

        void remove(ecs::Registry&, ecs::Entity) noexcept
        {
            ++removed;
        }
    };
} // namespace

int main()
{
    static_cast<void>(object::ObjectRuntime::instance());
    auto engine = take(engine::EngineContext::create({2, 64, {64}}, 64));
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("861ca828-f86d-437a-b73d-de88a164bac4")};
    ProjectManifest manifest{1, ids("project"), "Author document"};
    auto context = take(fixture::createContext(
        *engine,
        {manifest.name, std::filesystem::current_path()},
        manifest,
        [](EditorComposition&) noexcept -> FrameworkResult<void> { return {}; }
    ));
    std::vector<ecs::ComponentSchema> schema_values;
    for (auto group : {ecs::hierarchyComponentSchemas(), ecs::transformComponentSchemas()})
    {
        schema_values.insert(schema_values.end(), group.begin(), group.end());
    }
    auto schemas = take(ecs::ComponentSchemaSet::build(std::move(schema_values)));
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
    const std::array names{world::worldDataSchemaId("lux.ecs.Parent"), world::worldDataSchemaId("lux.ecs.Transform3D")};
    auto package = take(scene::createScenePackage(
        asset::AssetId{ids("scene")},
        "Authoring",
        names,
        std::make_shared<const simulation::SimulationDescription>(),
        take(std::move(description).buildResolved())
    ));
    auto world = std::shared_ptr<const world::WorldDescription>{package.world, &package.world->data()};
    auto source = take(process::world_loading::makeWorldMemoryStorageSource(world, package.volumes));
    scene::WorldLoadingServices services{source, context->tasks()};
    services.initial_partitions = package.partitions;
    auto provider =
        scene::makeSceneCapabilityProvider<scene::WorldLoadingServices>("loading", "lux.world.loading", services);
    const simulation::SimulationSystemRegistry simulation_systems;
    auto& runtime = engine->sceneRuntime();
    auto lease = take(runtime.builder()
                          .setDescription({package.scene, &package.scene->data()})
                          .setWorld(world)
                          .setSimulation({package.simulation, &package.simulation->data()})
                          .setRegistrations(schemas, simulation_systems, std::span(&loading, 1))
                          .setProviders(std::span(&provider, 1))
                          .build());
    const auto instance = take(context->adoptScene(std::move(lease)));
    assert(runtime.pauseSimulation(instance));
    auto owned = std::make_unique<SceneDocument>(runtime, instance, schemas, package);
    auto* document = owned.get();
    assert(!document->dirty());
    auto& registry = take(runtime.borrowInstance(instance)).get();
    auto& residency = registry.ctx().get<scene::WorldResidency>();
    const world::WorldObjectId first{ids("first")}, second{ids("second")};
    ecs::Transform3D transform;
    auto transform_value = encode(schemas, residency.identities(), transform);
    SceneCreateObjects create{{{first, {0}, {transform_value}}, {second, {0}, {transform_value}}}};
    assert(document->apply({0, create}));
    assert(document->revision() == 1 && document->dirty() && document->objects()->size() == 2);
    Updates updates;
    entt::scoped_connection replaced = registry.on_update<ecs::Transform3D>().connect<&Updates::replace>(updates);
    entt::scoped_connection added = registry.on_construct<ecs::Transform3D>().connect<&Updates::create>(updates);
    entt::scoped_connection removed = registry.on_destroy<ecs::Transform3D>().connect<&Updates::remove>(updates);
    transform.translation.x() = 42;
    SceneWriteComponents change{{{first, transform_value.schema, encode(schemas, residency.identities(), transform)}}};
    assert(document->apply({1, change}));
    assert(updates.replaced == 1 && updates.created == 0 && updates.removed == 0);
    assert(registry.get<ecs::Transform3D>(residency.identities().entity(first)).translation.x() == 42);
    auto stale = document->apply({1, change});
    assert(!stale && std::get<ESceneEditError>(stale.error()) == ESceneEditError::STALE_REVISION);
    auto invalid_batch = change;
    invalid_batch.components.front().value = transform_value;
    auto invalid = transform_value;
    invalid.version = 999;
    invalid_batch.components.push_back({second, invalid.schema, invalid});
    assert(!document->apply({2, invalid_batch}));
    assert(document->revision() == 2 && updates.replaced == 1);
    assert(registry.get<ecs::Transform3D>(residency.identities().entity(first)).translation.x() == 42);
    assert(document->undo());
    assert(registry.get<ecs::Transform3D>(residency.identities().entity(first)).translation.x() == 0);
    assert(document->redo());
    assert(updates.replaced == 3 && updates.created == 0 && updates.removed == 0);
    const auto parent = encode(schemas, residency.identities(), ecs::Parent{residency.identities().entity(first)});
    assert(document->apply({document->revision(), SceneWriteComponents{{{second, parent.schema, parent}}}}));
    const auto cycle = encode(schemas, residency.identities(), ecs::Parent{residency.identities().entity(second)});
    auto cycle_result = document->apply({document->revision(), SceneWriteComponents{{{first, cycle.schema, cycle}}}});
    assert(!cycle_result && std::get<ESceneEditError>(cycle_result.error()) == ESceneEditError::HIERARCHY_CYCLE);
    assert(!registry.all_of<ecs::Parent>(residency.identities().entity(first)));
    auto nested = [&](ecs::Registry&) noexcept
    {
        assert(!runtime.borrowInstance(instance));
        assert(!runtime.driveFrame());
    };
    assert(runtime.editInstance(instance, nested));
    const auto captured_revision = document->revision();
    auto captured = take(document->capture());
    assert(document->apply({captured_revision, change}));
    assert(document->saved(captured, captured_revision) && document->dirty());
    auto saved = take(document->capture());
    assert(document->saved(saved, document->revision()) && !document->dirty());
    const auto wire = take(scene::encodeScenePackage(saved, 1U << 20U));
    const auto bytes = cxx::SharedBytes<>::copyOf(wire);
    auto reopened = take(scene::decodeScenePackage(bytes));
    assert(reopened.partitions[0]->objectCount() == 2);
    assert(document->apply({document->revision(), SceneEraseObjects{{first, second}}}));
    assert(document->objects()->empty());
    assert(document->undo() && document->objects()->size() == 2);
    assert(
        registry.get<ecs::Parent>(residency.identities().entity(second)).entity == residency.identities().entity(first)
    );
    document->setReadOnly(true);
    assert(!document->apply({document->revision(), change}));
    document->setReadOnly(false);
    replaced.release();
    added.release();
    removed.release();
    const auto retirement = take(context->retireScene(instance));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!retirement.complete())
    {
        assert(std::chrono::steady_clock::now() < deadline);
        assert(engine->execution().dispatchMainCompletions());
        assert(runtime.driveFrame(scene::ESceneDriveMode::MAINTENANCE));
    }
    owned.reset();
    assert(runtime.instanceCount() == 0);
    context->close();
    while (!context->finishClose())
    {
        assert(engine->execution().dispatchMainCompletions());
        assert(runtime.driveFrame(scene::ESceneDriveMode::MAINTENANCE));
        assert(std::chrono::steady_clock::now() < deadline);
    }
    context.reset();
    std::puts("Scene document: atomic edits, update signals, stale drafts, hierarchy, undo, save capture and "
              "retirement verified");
}
