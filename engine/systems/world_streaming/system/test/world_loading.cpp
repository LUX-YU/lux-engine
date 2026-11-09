#include <lux/engine/Engine.hpp>
#include <lux/engine/Init.hpp>
#include <lux/engine/EngineMetaFunctionRegistry.hpp>
#include <lux/engine/GeometryDeclaration.hpp>
#include <lux/engine/PartitionDeclaration.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/core/execution/ExecutionResult.hpp>
#include <lux/engine/ecs/HierarchySchema.hpp>
#include <lux/engine/ecs/Parent.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <lux/engine/scene/SceneContent.hpp>
#include <lux/engine/scene/ScenePersistence.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/toolchain/scene/SceneStreamingBuild.hpp>
#include <lux/engine/resource/asset/storage/directory/DirectoryAssetProvider.hpp>
#include <lux/engine/world/World.hpp>
#include <lux/engine/world/ObserverComponent.hpp>
#include <lux/engine/world/LifecyclePolicyComponent.hpp>

#include <array>
#include <cassert>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <latch>
#include <semaphore>

using namespace lux;
using namespace std::chrono_literals;

namespace
{
    template <class T> T identity(std::uint8_t value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return T{uuids::uuid{bytes}};
    }

    template <class Sender> auto wait(execution::ExecutionRuntime& execution, Sender sender)
    {
        std::optional<typename execution::detail::ExecutionTypes<Sender>::Result> result;
        assert(execution.spawn(stdexec::then(
            execution::asResult(stdexec::starts_on(execution.mainScheduler(), std::move(sender))),
            [&](auto value) noexcept { result.emplace(std::move(value)); }
        )));
        assert(execution.waitUntil([&]() noexcept { return result.has_value(); }));
        return std::move(*result);
    }

    void materialization(const ecs::ComponentSchemaSet& schemas)
    {
        world::World source{world::WorldDescription{"Capture", {}}};
        auto& authored = source.registry();
        const std::array entities{authored.create(), authored.create()};
        for (std::size_t i = 0; i < 2; ++i)
        {
            authored.emplace<world::WorldObjectId>(entities[i], identity<world::WorldObjectId>(i + 1));
            authored.emplace<ecs::Transform3D>(entities[i]);
        }
        authored.emplace<ecs::Parent>(entities[0], entities[1]);
        const auto* transform = schemas.find(cxx::make_basic_type_info<ecs::Transform3D>());
        const auto* parent = schemas.find(cxx::make_basic_type_info<ecs::Parent>());
        auto transform_capture = transform->capture(authored, entities[0], {});
        auto parent_capture = parent->capture(authored, entities[0], {});
        assert(transform_capture && parent_capture);
        auto transform_bytes = transform_capture->encode(authored, 4096);
        auto parent_bytes = parent_capture->encode(authored, 4096);
        assert(transform_bytes && parent_bytes);
        std::array first{scene::WorldComponentInput{transform, 1, *transform_bytes},
                         scene::WorldComponentInput{parent, 1, *parent_bytes}};
        std::array second{scene::WorldComponentInput{transform, 1, *transform_bytes}};
        const std::array input{
            scene::WorldObjectInput{identity<world::WorldObjectId>(1), first},
            scene::WorldObjectInput{identity<world::WorldObjectId>(2), second}
        };
        world::World destination{world::WorldDescription{"Prepared", {}}};
        auto& registry = destination.registry();
        auto& identities = destination.identities();
        unsigned constructed = 0;
        auto event = [&](ecs::Registry&, ecs::Entity) noexcept { ++constructed; };
        entt::scoped_connection connection = registry.on_construct<ecs::Transform3D>()
            .connect<&decltype(event)::operator()>(event);
        const auto missing = scene::WorldMaterializer::prepareObjects(registry, identities, std::span(input).first(1));
        assert(!missing && missing.error().component.code == ecs::EComponentDecodeError::UNRESOLVED_REFERENCE);
        assert(missing.error().component.reference == input[1].id);
        second[0].version = 99;
        const auto invalid = scene::WorldMaterializer::prepareObjects(registry, identities, input);
        assert(!invalid && invalid.error().object == 1);
        assert(invalid.error().component.code == ecs::EComponentDecodeError::UNSUPPORTED_VERSION);
        second[0].version = 1;
        const auto capacity = scene::WorldMaterializer::prepareObjects(registry, identities, input, 1);
        assert(!capacity && capacity.error().code == scene::EWorldMaterializeError::CAPACITY);
        assert(constructed == 0 && identities.size() == 0);
        {
            auto discarded = scene::WorldMaterializer::prepareObjects(registry, identities, input, 4096);
            assert(discarded && !registry.valid(discarded->entities()[0]));
        }
        assert(constructed == 0 && identities.size() == 0);
        auto prepared = scene::WorldMaterializer::prepareObjects(registry, identities, input, 4096);
        assert(prepared && prepared->entities().size() == 2);
        assert(prepared->componentBytes() == 2 * sizeof(ecs::Transform3D) + sizeof(ecs::Parent));
        assert(prepared->references().size() == 1 && constructed == 0);
        prepared->commit(registry, identities);
        const auto a = identities.entity(input[0].id), b = identities.entity(input[1].id);
        assert(registry.valid(a) && registry.valid(b) && constructed == 2);
        assert(registry.get<ecs::Parent>(a).entity == b);
        const auto duplicate = scene::WorldMaterializer::prepareObjects(registry, identities, input);
        assert(!duplicate && duplicate.error().code == scene::EWorldMaterializeError::DUPLICATE_OBJECT);
        std::vector<ecs::Entity> references;
        assert(parent->visit_references(registry, a, {&references, +[](void* data, ecs::Entity entity) noexcept
        { static_cast<std::vector<ecs::Entity>*>(data)->push_back(entity); }}));
        assert(references == std::vector{b} && constructed == 2);
        // Generic materialization still supports references; current Scene storage explicitly does not.
        std::vector<scene::SceneObjectData> captured;
        assert(!scene::captureSceneBlock(source, entities, schemas, captured) && captured.empty());
    }

    void observerCodec(const ecs::ComponentSchemaSet& schemas)
    {
        ecs::Registry registry;
        ecs::WorldEntityMap identities;
        const auto source = registry.create(), destination = registry.create();
        registry.emplace<world::ObserverComponent>(source, 23.0);
        const auto* schema = schemas.find(cxx::make_basic_type_info<world::ObserverComponent>());
        assert(schema && schema->capture && schema->decode_value && schema->visit_references);
        auto captured = schema->capture(registry, source, {});
        assert(captured);
        auto bytes = captured->encode(registry, 4096);
        assert(bytes && schema->decode_emplace(registry, identities, destination, schema->version, *bytes));
        assert(registry.get<world::ObserverComponent>(destination).range == 23.0);
        bytes->pop_back();
        assert(!schema->decode_emplace(registry, identities, destination, schema->version, *bytes));
        assert(registry.get<world::ObserverComponent>(destination).range == 23.0);
        assert(schema->visit_references(registry, destination, {nullptr,
            +[](void*, ecs::Entity) noexcept { assert(false && "Observer range is not an Entity reference"); }}));
    }
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    constexpr std::size_t limit = 4U << 20U;
    const auto directory = std::filesystem::absolute(argv[1]) / "current-scene";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    auto provider = asset::DirectoryAssetProvider::create(directory);
    assert(provider);
    asset::MountLease mount;
    std::vector<std::unique_ptr<asset::AssetCodec>> codecs;
    codecs.push_back(scene::createSceneCodec());
#if defined(LUX_ENGINE_WITH_RENDER)
    render::RenderServerConfig rendering;
    rendering.channel_name = "test.world.loading";
    auto initialized = engine::init(
        {2, 64, {16}, execution::BlockingSchedulerConfig{1, 32}},
        {limit, limit, 8}, rendering, {}, {}, std::move(codecs)
    );
#else
    auto initialized = engine::init(
        {2, 64, {16}, execution::BlockingSchedulerConfig{1, 32}}, {limit, limit, 8}, {}, std::move(codecs)
    );
#endif
    assert(initialized);
    auto context = std::move(*initialized);
    auto& execution = context->execution();
    auto& assets = context->assets();
    mount = std::move(*context->assetVfs().mount({"/Scenes", *provider}));
    std::vector<ecs::ComponentSchema> values;
    const std::array schema_groups{
        ecs::transformComponentSchemas(), ecs::hierarchyComponentSchemas(), world::worldComponentSchemas()
    };
    for (auto group : schema_groups)
        values.insert(values.end(), group.begin(), group.end());
    auto schemas = ecs::ComponentSchemaSet::build(std::move(values));
    assert(schemas);
    materialization(*schemas);
    observerCodec(*schemas);
    engine::EngineMetaFunctionRegistry metadata;
    world::PartitionDeclaration partitioner;
    partitioner.name = "uniform";
    partitioner.configuration_version = 1;
    partitioner.dimension = scene::ESceneSpatialDimension::THREE;
    partitioner.build_index = &engine::toolchain::buildSpatialIndex<scene::ESceneSpatialDimension::THREE>;
    assert(metadata.addPartitioner(std::move(partitioner)));
    scene::GeometryDeclaration geometry;
    geometry.name = "fixture.mesh";
    assert(metadata.addGeometry(std::move(geometry)));
    scene::SceneDescription description;
    description.world.name = "Loading";
    world::SceneGridConfiguration grid;
    grid.cell_size = 100;
    std::vector<std::byte> configuration;
    assert(serialization::makePortableValueCodec<world::SceneGridConfiguration>().encode(&grid, configuration));
    description.world.supported_geometries.push_back({"fixture.mesh", {"uniform", 1, configuration}});
    const auto first = identity<world::WorldObjectId>(1), second = identity<world::WorldObjectId>(2);
    std::vector<scene::SceneBlockUpdate> updates{
        {0, {{first, {}}}, {{first, {{0, {{0, 0, 0}, {1, 1, 1}}}}}}},
        {1, {{second, {}}}, {{second, {{0, {{500, 0, 0}, {501, 1, 1}}}}}}}
    };
    const auto publish = [&](std::uint8_t id, const std::vector<scene::SceneBlockUpdate>& contents)
    {
        auto streaming = wait(execution, engine::toolchain::buildSceneStreaming(
            assets, {}, description, contents, metadata, limit
        ));
        assert(streaming);
        return wait(execution, scene::createSceneAsset(
            assets, "/Scenes/source" + std::to_string(id),
            {.id = identity<asset::AssetId>(id), .type = scene::SceneAsset::asset_type},
            description, contents, *streaming
        ));
    };
    auto source = publish(20, updates);
    assert(source);
    const auto make_scene = [&](asset::AssetId source_id, scene::WorldLoadingLimits limits, bool all)
    {
        auto world = std::make_unique<world::World>(description.world);
        auto loader = scene::WorldLoadingSystem::create(*context, *world, *schemas, source_id, 0, all, limits);
        assert(loader && (*loader)->stage() == scene::ESceneSystemStage::BEFORE_SIMULATION);
        auto* borrowed = loader->get();
        auto scene = std::make_unique<scene::SceneInstance>(
            std::move(world), std::make_unique<simulation::Simulation>()
        );
        scene->add(std::move(*loader));
        return std::pair{std::move(scene), borrowed};
    };
    // Existing observers fold into subscriptions; overlapping demand does not duplicate objects or IO adoption.
    {
        engine::Engine engine{*context};
        auto [candidate, loader] = make_scene(source->id(), {}, false);
        auto& world = candidate->world();
        auto& registry = world.registry();
        const auto a = registry.create(), b = registry.create();
        for (auto e : {a, b})
        {
            registry.emplace<ecs::Transform3D>(e);
            registry.emplace<world::ObserverComponent>(e, 10.0);
        }
        auto id = engine.addScene(candidate, true);
        assert(id && wait(execution, engine.whenSceneInitialized(*id)));
        std::vector<std::uint32_t> groups;
        assert(loader->queryGroups({{-10, -10, -10}, {10, 10, 10}}, groups) && groups == std::vector<std::uint32_t>{0});
        assert(!loader->retry(UINT32_MAX));
        unsigned phase = 0, steady_rounds = 0;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        ecs::Entity resident = ecs::NullEntity;
        engine.addEngineStageCallback([&]() noexcept -> engine::Engine::Result
        {
            assert(std::chrono::steady_clock::now() < deadline && loader->status());
            const auto one = world.identities().entity(first), two = world.identities().entity(second);
            if (phase == 0 && one != ecs::NullEntity)
            {
                resident = one;
                assert(two == ecs::NullEntity && world.identities().size() == 1);
                registry.remove<world::ObserverComponent>(a);
                phase = 1;
            }
            else if (phase == 1 && ++steady_rounds == 4)
            {
                assert(one == resident && world.identities().size() == 1);
                registry.emplace<world::LifecyclePolicyComponent>(one, world::ELifecyclePolicy::MANUAL);
                registry.patch<ecs::Transform3D>(b, [](auto& value) noexcept { value.translation.x() = 500; });
                phase = 2;
            }
            else if (phase == 2 && two != ecs::NullEntity)
            {
                assert(one == resident && world.identities().size() == 2);
                registry.patch<world::LifecyclePolicyComponent>(one, [](auto& value) noexcept
                { value.policy = world::ELifecyclePolicy::AUTOMATIC; });
                phase = 3;
            }
            else if (phase == 3 && one == ecs::NullEntity)
            {
                assert(!registry.valid(resident) && two != ecs::NullEntity);
                registry.destroy(b);
                phase = 4;
            }
            else if (phase == 4 && two == ecs::NullEntity)
            {
                assert(world.identities().size() == 0);
                phase = 5;
                engine.requestStop();
            }
            return {};
        }, engine::EEngineStage::END_UPDATE);
        assert(engine.run() && phase == 5);
        assert(wait(execution, engine.removeScene(*id)));
    }
    // All-at-once initialization adopts before publishing initialized; no camera is required.
    {
        engine::Engine engine{*context};
        auto [candidate, loader] = make_scene(source->id(), {}, true);
        auto& world = candidate->world();
        auto id = engine.addScene(candidate, true);
        assert(id && wait(execution, engine.whenSceneInitialized(*id)) && loader->status());
        const bool both_resident = world.identities().entity(first) != ecs::NullEntity &&
                                   world.identities().entity(second) != ecs::NullEntity;
        assert(both_resident);
        assert(wait(execution, engine.removeScene(*id)));
    }
    // One complete group containing two objects cannot be partially adopted under a one-entity budget.
    auto together = updates;
    together[0].objects.push_back(together[1].objects.front());
    together[0].summaries.push_back(together[1].summaries.front());
    together.resize(1);
    auto grouped = publish(21, together);
    assert(grouped);
    for (const std::size_t capacity : {1u, 2u})
    {
        engine::Engine engine{*context};
        scene::WorldLoadingLimits limits;
        limits.entities = capacity;
        auto [candidate, loader] = make_scene(grouped->id(), limits, true);
        auto& world = candidate->world();
        auto id = engine.addScene(candidate, true);
        assert(id);
        auto result = wait(execution, engine.whenSceneInitialized(*id));
        if (capacity == 1)
        {
            assert(!result && result.error().domainFailure()->type == error::errorId("lux.scene.content.capacity"));
            assert(world.identities().size() == 0);
        }
        else assert(result && world.identities().size() == 2);
        assert(wait(execution, engine.removeScene(*id)));
    }
    // Unknown or future Schema is rejected, never retained as opaque runtime content.
    const auto* transform = schemas->find(cxx::make_basic_type_info<ecs::Transform3D>());
    for (const bool unknown : {false, true})
    {
        auto invalid = together;
        invalid[0].objects[1].components.push_back({
            unknown ? ecs::componentSchemaId("removed.schema") : transform->id, unknown ? 1u : 99u, {}
        });
        auto bad = publish(unknown ? 23 : 22, invalid);
        assert(bad);
        engine::Engine engine{*context};
        auto [candidate, loader] = make_scene(bad->id(), {}, true);
        auto& world = candidate->world();
        auto id = engine.addScene(candidate, true);
        assert(id);
        auto result = wait(execution, engine.whenSceneInitialized(*id));
        assert(!result && world.identities().size() == 0);
        assert(wait(execution, engine.removeScene(*id)));
    }
    // A transient content read failure keeps its error identity and publishes no partial group.
    // Repair the same bytes behind the frozen source, then explicitly retry without replacing the loader.
    {
        auto snapshot = wait(execution, assets.read(*source));
        assert(snapshot);
        const auto content = directory / (*snapshot)->content_file;
        std::ifstream input(content, std::ios::binary);
        const std::vector<char> saved{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        input.close();
        assert(!saved.empty());
        engine::Engine engine{*context};
        auto [candidate, loader] = make_scene(source->id(), {}, false);
        auto& world = candidate->world();
        auto& registry = world.registry();
        const auto observer = registry.create();
        registry.emplace<ecs::Transform3D>(observer);
        registry.emplace<world::ObserverComponent>(observer, 10.0);
        auto id = engine.addScene(candidate, true);
        assert(id && wait(execution, engine.whenSceneInitialized(*id)));
        std::filesystem::resize_file(content, 0);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (loader->status())
        {
            assert(std::chrono::steady_clock::now() < deadline && execution.dispatch());
            const auto progressed = loader->execute();
            if (!progressed) assert(progressed.error().type == engine::assets::kAssetReadFailure);
        }
        assert(loader->status().error().type == engine::assets::kAssetReadFailure);
        assert(world.identities().size() == 0);
        {
            std::ofstream output(content, std::ios::binary | std::ios::trunc);
            output.write(saved.data(), static_cast<std::streamsize>(saved.size()));
            assert(output.good());
        }
        assert(loader->retry(0));
        while (world.identities().entity(first) == ecs::NullEntity)
        {
            assert(std::chrono::steady_clock::now() < deadline && execution.dispatch());
            assert(loader->execute());
        }
        assert(world.identities().size() == 1 && loader->status());
        assert(wait(execution, engine.removeScene(*id)));
    }
    // Block IO after adoption starts. Removing the Scene settles the admitted read before deleting its Registry.
    {
        engine::Engine engine{*context};
        auto [candidate, loader] = make_scene(source->id(), {}, false);
        auto& registry = candidate->world().registry();
        const auto observer = registry.create();
        registry.emplace<ecs::Transform3D>(observer);
        registry.emplace<world::ObserverComponent>(observer, 10.0);
        auto id = engine.addScene(candidate, true);
        assert(id && wait(execution, engine.whenSceneInitialized(*id)));
        std::latch occupied{1};
        std::binary_semaphore release{0};
        assert(execution.spawn(stdexec::then(
            execution::asResult(stdexec::schedule(*execution.blockingIoScheduler())),
            [&](auto ready) noexcept { assert(ready); occupied.count_down(); release.acquire(); }
        )));
        occupied.wait();
        assert(loader->execute()); // A test-owned Main safe point, outside all graph/observer traversals.
        assert(execution.dispatch());
        bool removed = false;
        assert(execution.spawn(stdexec::then(
            execution::asResult(stdexec::starts_on(execution.mainScheduler(), engine.removeScene(*id))),
            [&](auto result) noexcept { assert(result); removed = true; }
        )));
        assert(execution.dispatch() && !removed);
        release.release();
        assert(execution.waitUntil([&]() noexcept { return removed; }));
    }
    std::puts("PASS materialization, observers, demand overlap, capacity, strict Schema and removal with pending IO");
}
