#include <lux/cxx/core/scope_exit.hpp>
#include "support/ContextFixture.hpp"
#include <lux/engine/Engine.hpp>
#include <lux/engine/editor/SceneDocument.hpp>
#include <lux/engine/process/world_loading/WorldFileStorageSource.hpp>
#include <lux/engine/scene/ScenePackageFile.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/StreamingPolicy.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <lux/engine/spatial/Spatial3DPartitionIndex.hpp>
#include <lux/engine/world/SinglePartitionAlgorithm.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <source_location>
#include <thread>

using namespace lux;
using namespace lux::editor;
namespace ecs = ecs;
namespace loading = process::world_loading;

namespace
{
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("e7583f16-0c69-4f45-9bfc-c855668ad58f")};

    template <class T> auto take(T result, std::source_location location = std::source_location::current())
    {
        if (!result)
        {
            std::fprintf(stderr, "Failure at %s:%u\n", location.file_name(), location.line());
            if constexpr (requires { result.error().code; })
            {
                std::fprintf(stderr, "Error code: %u\n", static_cast<unsigned>(result.error().code));
            }
        }
        assert(result);
        return std::move(*result);
    }

    template <class T> scene::SceneComponentData encode(const ecs::ComponentSchemaSet& schemas, const T& value)
    {
        ecs::WorldEntityMap identities;
        const auto& schema = *schemas.find(cxx::make_basic_type_info<T>());
        auto captured = schema.capture_value(&value, schema.code_lifetime);
        return {schema.id, schema.version, take(captured.encode(identities, 8192))};
    }

    std::shared_ptr<const world::WorldPartitionData> partitionData(
        const scene::ScenePackage& package,
        std::uint32_t ordinal,
        std::span<const scene::SceneObjectData> values
    )
    {
        const auto& world = package.world->data();
        std::vector<std::vector<world::WorldEncodedDataRecord>> data(values.size());
        std::vector<world::WorldEncodedObjectRecord> objects;
        for (std::size_t i{}; i < values.size(); ++i)
        {
            for (const auto& component : values[i].components)
            {
                const auto found =
                    std::ranges::find(world.schemas(), component.schema.name, &world::WorldDataSchemaId::name);
                assert(found != world.schemas().end());
                data[i].push_back(
                    {static_cast<std::uint32_t>(found - world.schemas().begin()), component.version, component.payload}
                );
            }
            std::ranges::sort(data[i], {}, &world::WorldEncodedDataRecord::schema_ordinal);
            objects.push_back({values[i].id, data[i]});
        }
        auto wire = take(world::encodeWorldPartitionData({ordinal}, objects));
        auto decoded = take(world::decodeWorldPartitionData(
            wire,
            world.bundleId(),
            world.generation(),
            {ordinal},
            package.partitions[ordinal]->id(),
            static_cast<std::uint32_t>(world.schemas().size()),
            1U << 20U
        ));
        return std::make_shared<const world::WorldPartitionData>(std::move(decoded));
    }

    void writeVolume(const std::filesystem::path& directory, const scene::ScenePackage& package)
    {
        std::filesystem::create_directories(directory);
        const auto& volume = package.volumes.front();
        std::ofstream file(directory / package.world->data().storageVolumes().front().member_name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(volume.data()), volume.size());
        file.close();
        assert(file.good());
    }

    // Deterministic pause at the existing external port. Reads still use the real
    // file source and the Engine's shared blocking scheduler; no fixture thread pool.
    class HeldReads final : public async::TOperationPort<loading::ReadWorldStorageRange>::Endpoint
    {
        using Range = loading::ReadWorldStorageRange;
        using Outcome = async::OperationOutcome<Range>;

        struct Request final
        {
            Range range;
            void* state;
            void (*complete)(void*, Outcome&&) noexcept;
            async::SubmitOptions options;
        };

        std::unique_ptr<loading::WorldFileStorageSource> files_;
        std::mutex mutex_;
        std::vector<Request> held_;
        bool hold_{};

    public:
        HeldReads(
            std::shared_ptr<const world::WorldDescription> world,
            std::filesystem::path path,
            execution::ExecutionRuntime& execution,
            execution::TaskScope& tasks
        )
            : files_(take(loading::WorldFileStorageSource::create(
                  std::move(world),
                  std::move(path),
                  take(execution.blockingIoScheduler()),
                  execution,
                  {},
                  tasks.stopToken()
              )))
        {
        }

        async::SubmitResult submit(
            Range range,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            async::SubmitOptions options
        ) noexcept override
        {
            {
                std::lock_guard lock(mutex_);
                if (hold_)
                {
                    held_.push_back({range, state, complete, options});
                    return {};
                }
            }
            return files_->source().readPort().submit(range, state, complete, options);
        }

        void hold()
        {
            std::lock_guard lock(mutex_);
            hold_ = true;
        }

        std::size_t pending()
        {
            std::lock_guard lock(mutex_);
            return held_.size();
        }

        void release()
        {
            std::vector<Request> requests;
            {
                std::lock_guard lock(mutex_);
                hold_ = false;
                requests.swap(held_);
            }
            for (const auto& request : requests)
            {
                const auto accepted =
                    files_->source().readPort().submit(request.range, request.state, request.complete, request.options);
                if (!accepted)
                {
                    request.complete(
                        request.state,
                        cxx::unexpected(
                            async::TOperationFailure<loading::WorldStorageRuntimeFailure>::runtime(accepted.error())
                        )
                    );
                }
            }
        }
    };

    struct Queries final
    {
        std::array<spatial::Spatial3DPartitionIndex, 2> domains;
        std::array<ecs::Entity, 2> demands{ecs::NullEntity, ecs::NullEntity};

        std::vector<partition::PartitionOrdinal> query(std::uint32_t source) const
        {
            std::vector<partition::PartitionOrdinal> result;
            for (std::size_t i = source; i < domains.size(); ++i)
            {
                std::array<partition::PartitionOrdinal, 2> matches;
                const auto count = take(domains[i].query({{0, 0, 0}, {1, 1, 1}}, matches));
                result.insert(result.end(), matches.begin(), matches.begin() + count);
            }
            return result;
        }
    };

    struct QueryPolicy final
    {
        static constexpr system::SystemDataContract Outputs[]{
            {"lux.scene.WorldStreamingDemand", 1, system::ESystemDataKind::SCHEMA, system::ESystemDataPhase::STABLE}
        };
        static constexpr system::SystemTypeDescription Description{
            .canonical_name = "test.scene.query_policy",
            .version = 1,
            .configuration_schema_name = "test.QuerySelection",
            .configuration_schema_version = 1,
            .data_outputs = Outputs
        };
        ecs::Registry& registry;
        Queries& queries;
        std::uint32_t source;
        ecs::Entity entity;

        QueryPolicy(ecs::Registry& registry, Queries& queries, std::uint32_t source)
            : registry(registry), queries(queries), source(source), entity(registry.create())
        {
            queries.demands[source] = entity;
            registry.emplace<scene::WorldStreamingDemand>(entity, queries.query(source), true);
        }

        ~QueryPolicy() noexcept
        {
            registry.destroy(entity);
            queries.demands[source] = ecs::NullEntity;
        }
    };

    static_assert(scene::StreamingPolicy<QueryPolicy>);

    scene::SceneSystemRegistration queryRegistration()
    {
        static constexpr std::array requirements{
            scene::SceneDependencyRequirement{
                "queries", "test.domain_queries", cxx::make_basic_type_info<Queries>(), false
            }
        };
        return {
            .type = system::systemTypeId(QueryPolicy::Description.canonical_name),
            .cpp_type = cxx::make_basic_type_info<QueryPolicy>(),
            .description = &QueryPolicy::Description,
            .configuration = serialization::makePortableValueCodec<std::uint32_t>(),
            .requirements = requirements,
            .install = +[](scene::SceneSystemInstaller& installer, scene::SceneSystemDescription input
                        ) noexcept -> cxx::expected<void, scene::SceneSystemBuildFailure>
            {
                auto configuration = installer.decodeConfiguration<std::uint32_t>(input);
                auto* queries = installer.require<Queries>(input.instanceId(), "queries");
                if (!configuration || *configuration > 1 || !queries)
                {
                    return cxx::unexpected(
                        scene::SceneSystemBuildFailure{scene::ESceneSystemBuildError::INVALID_DESCRIPTION}
                    );
                }
                auto instance =
                    installer
                        .emplaceSystem<QueryPolicy>(input.instanceId(), installer.registry(), *queries, *configuration);
                if (!instance)
                {
                    return cxx::unexpected(instance.error());
                }
                return {};
            }
        };
    }

    void schemaMigration(const ecs::ComponentSchemaSet& schemas, const std::filesystem::path& directory)
    {
        assert(!schemas.find(ecs::componentSchemaId("lux.scene.Observer")));
        const std::array names{world::worldDataSchemaId("lux.scene.Observer")};
        auto empty = take(scene::createScenePackage(
            asset::AssetId{ids("legacy")},
            "Legacy demand",
            names,
            std::make_shared<const simulation::SimulationDescription>(),
            scene::SceneDescription{}
        ));
        auto payload = encode(schemas, scene::WorldStreamingDemand{{{0}}, false});
        payload.schema = ecs::componentSchemaId("lux.scene.Observer");
        const scene::SceneObjectData object{{ids("legacy-demand")}, {0}, {payload}};
        const std::array parts{partitionData(empty, 0, std::span(&object, 1))};
        auto legacy = take(scene::assembleScenePackage(empty, parts));
        const auto file = directory / "legacy-demand.scene";
        assert(scene::writeScenePackageAtomic(file, legacy, platform::EFileWriteMode::REPLACE, 1U << 24U));
        auto read = take(scene::readScenePackageFile(file, 1U << 24U));
        assert(scene::migrateScenePackageComponents(read, schemas));
        assert(read.world->data().schemas().front().name == "lux.scene.WorldStreamingDemand");
        auto migrated = read.partitions.front()->objectAt(0);
        scene::WorldStreamingDemand value;
        assert(scene::worldStreamingDemandCodec().decode(migrated.payloadAt(0), &value));
        assert(value.partitions.size() == 1 && !value.required && migrated.id() == object.id);
        assert(scene::writeScenePackageAtomic(file, read, platform::EFileWriteMode::REPLACE, 1U << 24U));
        auto reopened = take(scene::readScenePackageFile(file, 1U << 24U));
        assert(reopened.world->data().schemas().front().name == "lux.scene.WorldStreamingDemand");
        const auto generation = reopened.world->data().generation();
        assert(scene::migrateScenePackageComponents(reopened, schemas));
        assert(reopened.world->data().generation() == generation); // No second conversion.

        auto unsupported = object;
        unsupported.components.front().version = 99;
        const std::array bad_parts{partitionData(empty, 0, std::span(&unsupported, 1))};
        auto bad = take(scene::assembleScenePackage(empty, bad_parts));
        const auto previous = bad.world;
        auto rejected = scene::migrateScenePackageComponents(bad, schemas);
        assert(
            !rejected && rejected.error().code == scene::EScenePackageError::SCHEMA_MIGRATION && bad.world == previous
        );
        std::puts("PASS explicit demand Schema migration: rewritten ID, payload, save/reopen, bad version unchanged");
    }
} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    auto& objects = object::ObjectRuntime::instance();
    assert(objects.isCurrent());
    auto engine = take(engine::Engine::create({2, 128, {64}, execution::BlockingSchedulerConfig{2, 64}}, 128));
    cxx::scope_exit close_engine([&]() noexcept { assert(engine->close()); });
    const auto directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    ProjectManifest manifest{1, ids("project"), "World domains"};
    auto context = take(fixture::createContext(
        *engine,
        {manifest.name, directory},
        manifest,
        [](EditorComposition&) noexcept -> FrameworkResult<void> { return {}; }
    ));
    std::vector<ecs::ComponentSchema> schema_values;
    for (auto group : {ecs::transformComponentSchemas(), scene::worldLoadingComponentSchemas()})
    {
        schema_values.insert(schema_values.end(), group.begin(), group.end());
    }
    auto schemas = take(ecs::ComponentSchemaSet::build(std::move(schema_values)));
    schemaMigration(schemas, directory);
    std::vector<scene::GeometryDeclaration> geometries;
    for (auto [name, type] :
         {std::pair{"test.geometry.volume", cxx::make_basic_type_info<ecs::Transform3D>()},
          std::pair{"test.geometry.plane", cxx::make_basic_type_info<ecs::Transform2D>()}})
    {
        const auto& schema = *schemas.find(type);
        geometries.push_back(
            {
                name,
                1,
                {{schema.id, schema.version, schema.cpp_type}},
                cxx::make_basic_type_info<world::ObjectPartitionInput>()
            }
        );
    }
    const std::array algorithms{world::partitionDeclaration<world::SinglePartitionAlgorithm>()};
    const std::array domains{
        world::WorldDomainSelection{"volume", algorithms[0].identity},
        world::WorldDomainSelection{"plane", algorithms[0].identity}
    };
    const std::array registrations{scene::worldLoadingSystemRegistration(), queryRegistration()};
    scene::SceneDescriptionBuilder selected;
    assert(selected.selectGeometry({{geometries[0].name, 1}, "volume"}));
    assert(selected.selectGeometry({{geometries[1].name, 1}, "plane"}));
    for (std::uint32_t i{}; i < 3; ++i)
    {
        const auto& registration = registrations[i == 0 ? 0 : 1];
        std::vector<std::byte> configuration;
        if (i == 0)
        {
            const scene::WorldLoadingConfiguration value;
            assert(registration.configuration.encode(&value, configuration));
        }
        else
        {
            const std::uint32_t value = i - 1;
            assert(registration.configuration.encode(&value, configuration));
        }
        const auto& description = *registration.description;
        assert(selected.addSystem(
            {i + 1},
            "system" + std::to_string(i),
            registration.type,
            description.version,
            description.configuration_schema_name,
            description.configuration_schema_version,
            configuration
        ));
        if (i)
        {
            assert(selected.addDependency({i + 1}, {1}));
        }
    }
    auto description = take(std::move(selected).buildResolved());
    const std::array names{
        world::worldDataSchemaId("lux.ecs.Transform3D"),
        world::worldDataSchemaId("lux.ecs.Transform2D")
    };
    auto empty = take(scene::createScenePackage(
        asset::AssetId{ids("scene")},
        "Two domains",
        names,
        std::make_shared<const simulation::SimulationDescription>(),
        description,
        domains,
        algorithms
    ));
    const auto volume = encode(schemas, ecs::Transform3D{});
    const auto plane = encode(schemas, ecs::Transform2D{});
    const scene::SceneObjectData first{{ids("first")}, {0}, {volume, plane}, "volume"};
    const scene::SceneObjectData second{{ids("second")}, {1}, {plane}, "plane"};
    const std::array parts{
        partitionData(empty, 0, std::span(&first, 1)),
        partitionData(empty, 1, std::span(&second, 1))
    };
    auto package = take(scene::assembleScenePackage(empty, parts));
    auto duplicate = second;
    duplicate.id = first.id;
    const std::array duplicated{parts[0], partitionData(empty, 1, std::span(&duplicate, 1))};
    assert(!scene::assembleScenePackage(empty, duplicated));
    const auto file = directory / "two-domains.scene";
    assert(scene::writeScenePackageAtomic(file, package, platform::EFileWriteMode::REPLACE, 1U << 24U));
    package = take(scene::readScenePackageFile(file, 1U << 24U));
    writeVolume(directory, package);
    scene::SceneContentAllowance allowed{
        {{geometries[0].name, 1}, {geometries[1].name, 1}},
        {{algorithms[0].identity.id.name, 1}}
    };
    const auto content = take(scene::resolveSceneContent(
        allowed,
        package.scene->data().geometries(),
        package.world->data(),
        schemas,
        geometries,
        algorithms
    ));
    assert(scene::validateWorldObjectDomains(content, package.world->data(), geometries, package.partitions));

    Queries queries{
        {take(spatial::Spatial3DPartitionIndex::create({0, 0, 0}, 1, {{{0, 0, 0}, {0}}})),
         take(spatial::Spatial3DPartitionIndex::create({0, 0, 0}, 1, {{{0, 0, 0}, {1}}}))}
    };
    for (std::size_t i{}; i < queries.domains.size(); ++i)
    {
        assert(package.world->data().domainAt(queries.domains[i].entries().front().partition)->key == domains[i].key);
    }
    auto world = package.world->sharedData();
    auto reads = std::make_shared<HeldReads>(world, directory, engine->execution(), context->tasks());
    auto source =
        take(loading::WorldStorageSource::create(world, async::TOperationPort<loading::ReadWorldStorageRange>{reads}));
    scene::WorldLoadingServices loading_services{source, context->tasks()};
    const std::array providers{
        scene::makeSceneDependency<scene::WorldLoadingServices>(
            "loading",
            "lux.world.loading",
            loading_services
        ),
        scene::makeSceneDependency<Queries>("queries", "test.domain_queries", queries)
    };
    simulation::SimulationSystemRegistry simulation;
    auto& runtime = engine->sceneRuntime();
    const auto create = [&]
    {
        auto builder = runtime.builder();
        builder.setDescription(package.scene->sharedData())
            .setWorld(world)
            .setSimulation(package.simulation->sharedData())
            .setRegistrations(schemas, simulation, registrations)
            .setProviders(providers)
            .setContentDeclarations(allowed, geometries, algorithms);
        assert(builder.validate());
        return take(context->createScene(builder));
    };
    const auto pump = [&]
    {
        assert(engine->execution().dispatchMainCompletions());
        assert(runtime.driveFrame());
    };
    const auto until = [&](auto ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ready())
        {
            pump();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    };
    auto instance = create();
    until(
        [&]
        {
            return take(runtime.borrowInstance(instance))
                       .get()
                       .ctx()
                       .get<scene::WorldResidency>()
                       .statistics()
                       .resident_entities == 2;
        }
    );
    auto& registry = take(runtime.borrowInstance(instance)).get();
    auto& residency = registry.ctx().get<scene::WorldResidency>();
    assert((registry.all_of<ecs::Transform2D, ecs::Transform3D>(residency.identities().entity(first.id))));
    const auto second_entity = residency.identities().entity(second.id);
    const auto withdraw = [&](std::size_t index, bool active)
    {
        auto edit = [&](ecs::Registry& values) noexcept
        {
            values.patch<scene::WorldStreamingDemand>(
                queries.demands[index],
                [&](auto& demand)
                {
                    demand.partitions = active ? queries.query(static_cast<std::uint32_t>(index))
                                               : std::vector<partition::PartitionOrdinal>{};
                }
            );
        };
        until([&] { return runtime.editInstance(instance, edit).has_value(); });
    };
    withdraw(0, false);
    until([&] { return residency.statistics().resident_entities == 1; });
    assert(residency.identities().entity(second.id) == second_entity); // Other source still owns its demand.
    withdraw(1, false);
    until([&] { return residency.statistics().resident_entities == 0; });
    withdraw(0, true);
    until([&] { return residency.statistics().resident_entities == 2; });
    auto document = take(SceneDocument::create(runtime, instance, schemas, package, geometries));
    const scene::SceneObjectData ambiguous{{ids("authored")}, {UINT32_MAX}, {volume, plane}};
    auto rejected = document->apply({0, SceneCreateObjects{{ambiguous}}});
    assert(
        !rejected &&
        std::get<scene::SceneContentFailure>(rejected.error()).code == scene::ESceneContentError::AMBIGUOUS_DOMAIN
    );
    auto authored = ambiguous;
    authored.primary_domain = "volume";
    assert(document->apply({0, SceneCreateObjects{{authored}}}));
    assert(document->objects()->size() == 3 && document->object(authored.id)->partition.value == 0);
    SceneWriteComponents move_without_request{{{authored.id, volume.schema, {}}}};
    assert(!document->apply({1, move_without_request}));
    assert(document->revision() == 1 && registry.all_of<ecs::Transform3D>(residency.identities().entity(authored.id)));
    assert(document->undo() && document->objects()->size() == 2);
    assert(document->redo() && document->objects()->size() == 3);
    auto saved = take(document->capture());
    assert(scene::writeScenePackageAtomic(file, saved, platform::EFileWriteMode::REPLACE, 1U << 24U));
    auto reopened = take(scene::readScenePackageFile(file, 1U << 24U));
    assert(reopened.partitions[0]->objectCount() == 2 && reopened.partitions[1]->objectCount() == 1);
    assert(scene::validateWorldObjectDomains(content, reopened.world->data(), geometries, reopened.partitions));
    document.reset();
    auto retirement = take(context->retireScene(instance));
    until([&] { return retirement.complete(); });
    assert(queries.demands[0] == ecs::NullEntity && queries.demands[1] == ecs::NullEntity);

    // Required corrupt component version rejects the entire wanted adoption set.
    auto invalid = second;
    invalid.components.front().version = 99;
    const std::array invalid_parts{parts[0], partitionData(empty, 1, std::span(&invalid, 1))};
    auto invalid_package = take(scene::assembleScenePackage(empty, invalid_parts));
    writeVolume(directory, invalid_package);
    auto invalid_reads = std::make_shared<HeldReads>(
        invalid_package.world->sharedData(),
        directory,
        engine->execution(),
        context->tasks()
    );
    loading_services.source = take(loading::WorldStorageSource::create(
        invalid_package.world->sharedData(),
        async::TOperationPort<loading::ReadWorldStorageRange>{invalid_reads}
    ));
    world = invalid_package.world->sharedData();
    instance = create();
    auto* loader = take(runtime.borrowSystem<scene::WorldLoadingSystem>(instance));
    until([&] { return !loader->status(); });
    assert(
        take(std::as_const(runtime).borrowInstance(instance))
            .get()
            .ctx()
            .get<scene::WorldResidency>()
            .statistics()
            .resident_entities == 0
    );
    retirement = take(context->retireScene(instance));
    until([&] { return retirement.complete(); });

    writeVolume(directory, package);
    loading_services.source = source;
    world = package.world->sharedData();
    reads->hold();
    instance = create();
    until([&] { return reads->pending() != 0; });
    retirement = take(context->retireScene(instance));
    pump();
    reads->release();
    until([&] { return retirement.complete(); });
    assert(runtime.instanceCount() == 0);
    context->close();
    until([&] { return context->finishClose(); });
    context.reset();
    std::puts("PASS Editor multi-domain: real queries/file reads/decode, one entity per ID, overlap/revoke, author "
              "undo/save, wrong-version atomic rejection, in-flight close");
}
