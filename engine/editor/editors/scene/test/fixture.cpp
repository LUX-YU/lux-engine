#include "run_system.hpp"
#include "scene_source_checks.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>
#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/TransformSystem.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <lux/engine/simulation/ecs/WorldComponentArchive.hpp>
#include <lux/engine/world/WorldAssetCodec.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>

template <class T> T identity(std::uint8_t tail)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = tail;
    return T{uuids::uuid{bytes}};
}

int main(int argc, char **argv)
{
    using namespace lux;
    using namespace lux::world;

    assert(argc >= 3 && argc <= 5);
    const std::filesystem::path root{argv[1]};
    if (std::string_view(argv[2]) == "verify-preservation")
    {
        SceneSourceChecks::verify(root);
        return 0;
    }
    if (std::string_view(argv[2]) == "inspect-render")
    {
        auto source = SceneSourceChecks::read(root / "Main.luxscene");
        auto bytes = std::make_shared<std::vector<std::byte>>(std::move(source.scene));
        auto scene_asset = asset::TAssetSerDeser<scene::SceneAsset>::decode(
            identity<asset::AssetId>(12), cxx::SharedBytes<>::fromOwner(bytes, *bytes),
            {16 * 1024 * 1024, 16 * 1024 * 1024, 128});
        assert(scene_asset);
        const auto registration = scene::builtinRenderSystemRegistration();
        const auto features = render::builtinRenderFeatureRegistrations();
        const auto &description = (*scene_asset)->data();
        for (std::size_t index{}; index < description.systemCount(); ++index)
        {
            const auto system = description.systemAt(index);
            if (system.type() != registration.type)
            {
                continue;
            }
            scene::RenderSystemConfiguration config;
            assert(registration.configuration.decode(system.configurationPayload(), &config));
            std::printf("render feature count=%zu page=%.0f\n", config.features.size(), config.coordinate_page_size);
            for (const auto &feature : config.features)
            {
                const auto found = std::ranges::find_if(
                    features, [&](const auto &entry) { return entry.factory.descriptor.type == feature.type; });
                assert(found != features.end());
                std::printf("feature=%.*s\n", static_cast<int>(found->factory.descriptor.canonical_name.size()), found->factory.descriptor.canonical_name.data());
            }
        }
        return 0;
    }
    const bool preservation = std::string_view(argv[2]) == "gpu-preservation";
    const bool observer = std::string_view(argv[2]) == "gpu-observer";
    std::filesystem::create_directories(root);
    const bool rendered = std::string_view(argv[2]).starts_with("gpu");
    const bool shadows = std::string_view(argv[2]) == "gpu-shadow";
    std::vector<WorldDataSchemaId> schemas{worldDataSchemaId("lux.ecs.Transform3D"),
                                           worldDataSchemaId("lux.ecs.Mesh3D"), worldDataSchemaId("lux.ecs.Light3D")};
    if (rendered)
    {
        schemas.push_back(worldDataSchemaId("lux.scene.Camera"));
    }
    if (observer)
    {
        schemas.push_back(worldDataSchemaId("lux.scene.Observer"));
    }
    if (std::string_view(argv[2]) == "gpu-hierarchy")
    {
        schemas.push_back(worldDataSchemaId("lux.ecs.Parent"));
    }
    if (preservation)
    {
        schemas.push_back(worldDataSchemaId("test.UnknownAuthorPayload"));
    }
    std::ranges::sort(schemas, WorldDataSchemaIdLess{});
    const auto ordinal = [&](std::string_view name) {
        return static_cast<std::uint32_t>(std::ranges::find(schemas, name, &WorldDataSchemaId::name) - schemas.begin());
    };
    simulation::ecs::Registry registry;
    simulation::ecs::WorldEntityMap identities;
    const auto encode = [&](const auto &component) {
        using Component = std::remove_cvref_t<decltype(component)>;
        const auto entity = registry.create();
        registry.emplace<Component>(entity, component);
        for (const auto registered :
             {simulation::ecs::transformComponentSchemas(), simulation::ecs::visualComponentSchemas(),
              scene::sceneRenderComponentSchemas(), scene::worldLoadingComponentSchemas()})
        {
            const auto found =
                std::ranges::find(registered, cxx::typeToken<Component>(), &simulation::ecs::ComponentSchema::cpp_type);
            if (found != registered.end())
            {
                assert(found->capture);
                auto capture = found->capture(registry, entity, found->code_lifetime);
                assert(capture);
                auto bytes = capture->encode(identities, 1024 * 1024);
                assert(bytes);
                registry.destroy(entity);
                return std::move(*bytes);
            }
        }
        std::abort();
    };
    std::array<std::array<std::vector<std::byte>, 4>, 4> payloads;
    std::array<std::vector<WorldEncodedDataRecord>, 4> data;
    const auto object_count = argc >= 4 ? std::stoul(argv[3]) : 4;
    assert(object_count >= 4 && object_count <= 4096);
    std::vector<WorldEncodedObjectRecord> objects(object_count);
    const auto seed = [](std::uint8_t tail) {
        std::array<std::uint8_t, 16> bytes{0x53, 0x56, 1};
        bytes.back() = tail;
        return asset::AssetId{bytes};
    };
    for (std::size_t index{}; index < 4; ++index)
    {
        simulation::ecs::Transform3D transform;
        if (index == 0)
        {
            transform.translation = {0, 1, 0};
        }
        if (index == 1)
        {
            transform.translation = {-2, 0.75, -1};
            transform.scale = {1.5, 1.5, 0.7};
        }
        if (index == 3)
        {
            transform.translation = {2, 5, 3};
            transform.rotation =
                Eigen::Quaterniond::FromTwoVectors(-Eigen::Vector3d::UnitZ(), -transform.translation.normalized());
        }
        payloads[index][0] = encode(transform);
        if (index < 3)
        {
            simulation::ecs::Mesh3D mesh;
            mesh.value.mesh = seed(index == 2 ? 11 : 10);
            mesh.value.material = seed(static_cast<std::uint8_t>(20 + index));
            if (index == 0 && std::string_view(argv[2]) == "gpu-bad-material")
            {
                mesh.value.material = seed(99);
            }
            payloads[index][1] = encode(mesh);
        }
        else
        {
            simulation::ecs::Light3D light;
            light.value.type = rdesc::ELightType::POINT;
            light.value.intensity = 3;
            light.value.range = 30;
            light.value.cast_shadow = shadows;
            payloads[index][1] = encode(light);
        }
        const std::uint32_t transform_version = std::string_view(argv[2]) == "gpu-bad-version" ? 99 : 1;
        data[index] = {
            WorldEncodedDataRecord{ordinal("lux.ecs.Transform3D"), transform_version, payloads[index][0]},
            WorldEncodedDataRecord{ordinal(index < 3 ? "lux.ecs.Mesh3D" : "lux.ecs.Light3D"), 2, payloads[index][1]}};
        if (rendered && index == 3)
        {
            scene::Camera camera;
            camera.primary = true;
            payloads[index][3] = encode(camera);
            data[index].push_back({ordinal("lux.scene.Camera"), 1, payloads[index][3]});
        }
        if (preservation)
        {
            payloads[index][2] = {std::byte{0}, std::byte{0xff}, static_cast<std::byte>(index), std::byte{0x41},
                                  std::byte{0}};
            data[index].push_back({ordinal("test.UnknownAuthorPayload"), 37, payloads[index][2]});
        }
        if (observer && index == 0)
        {
            payloads[index][2] = encode(scene::Observer{});
            data[index].push_back({ordinal("lux.scene.Observer"), 1, payloads[index][2]});
        }
        std::ranges::sort(data[index], {}, &WorldEncodedDataRecord::schema_ordinal);
        objects[index] = {identity<WorldObjectId>(static_cast<std::uint8_t>(index + 1)), data[index]};
    }
    for (std::size_t index = 4; index < objects.size(); ++index)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[14] = static_cast<std::uint8_t>((index + 1) >> 8);
        bytes[15] = static_cast<std::uint8_t>(index + 1);
        objects[index] = {WorldObjectId{uuids::uuid{bytes}}, {}};
    }
    std::vector<std::vector<std::byte>> partition_bytes;
    std::vector<WorldPartitionRecord> records;
    std::vector<WorldPartitionExtent> extents;
    if (observer)
    {
        assert(objects.size() == 4);
        std::swap(objects[1], objects[3]); // Bootstrap contains one Mesh and the game Camera/Light.
    }
    const std::uint32_t partition_count = preservation || observer ? 2 : 1;
    const std::uint32_t objects_per_partition = static_cast<std::uint32_t>(objects.size()) / partition_count;
    for (std::uint32_t index{}; index < partition_count; ++index)
    {
        auto partition_objects = std::span{objects}.subspan(index * objects_per_partition, objects_per_partition);
        std::ranges::sort(partition_objects, WorldObjectIdLess{}, &WorldEncodedObjectRecord::id);
        auto encoded = encodeWorldPartitionData(partition::PartitionOrdinal{index}, partition_objects);
        assert(encoded);
        partition_bytes.push_back(std::move(*encoded));
        records.push_back({identity<WorldPartitionId>(static_cast<std::uint8_t>(5 + index)), index, 1});
        extents.push_back({0, index + 1, 1});
    }
    auto table = encodeWorldPartitionTablePage(partition::PartitionOrdinal{0}, records, extents);
    assert(table);
    const auto bundle = identity<WorldBundleId>(6);
    const auto generation = identity<WorldBundleGeneration>(7);
    std::vector<WorldStorageChunkInput> chunks{
        {EWorldStorageChunkKind::PARTITION_TABLE_PAGE, EWorldStorageCodec::NONE, *table}};
    for (const auto &bytes : partition_bytes)
    {
        chunks.push_back({EWorldStorageChunkKind::WORLD_PARTITION_DATA, EWorldStorageCodec::NONE, bytes});
    }
    auto volume = encodeWorldStorageVolume(bundle, generation, 0, chunks);
    assert(volume);
    WorldDescriptionBuilder world_builder;
    assert(world_builder.setIdentity(bundle, generation, "Native World"));
    for (const auto &schema : schemas)
    {
        assert(world_builder.addSchema(schema));
    }
    assert(world_builder.setPartitioner({worldPartitionerId("test.native-single"), 1}, partition_count));
    assert(
        world_builder.addStorageVolume({"World.wvol", 1, static_cast<std::uint32_t>(chunks.size()), volume->size()}));
    assert(world_builder.addPartitionTablePage({partition::PartitionOrdinal{0}, partition_count, {0, 0}}));
    auto world_description = std::move(world_builder).build();
    assert(world_description);
    auto world_asset = WorldAsset::create(asset::AssetInfo{identity<asset::AssetId>(10)},
                                          std::make_shared<const WorldDescription>(std::move(*world_description)));
    assert(world_asset);
    simulation::SimulationDescriptionBuilder simulation_builder;
    auto transform = simulation::makeTransformSystemConfiguration(1024, {2048, 1024 * 1024});
    assert(transform && simulation_builder.addSystem(system::SystemInstanceId{1}, "transform",
                                                     simulation::transformSystemDescription(), *transform));
    if (std::string_view(argv[2]) == "gpu-dynamic" || observer)
    {
        assert(simulation_builder.addSystem(system::SystemInstanceId{3}, "motion", run_test::Motion::Description, {}));
        assert(simulation_builder.addExecutionDependency(
            simulation::SimulationExecutionPoint::task(system::SystemInstanceId{3}),
            simulation::SimulationExecutionPoint::task(system::SystemInstanceId{1})));
    }
    auto simulation_description = std::move(simulation_builder).build();
    assert(simulation_description);
    auto simulation_asset = simulation::SimulationAsset::create(
        asset::AssetInfo{identity<asset::AssetId>(11)},
        std::make_shared<const simulation::SimulationDescription>(std::move(*simulation_description)));
    assert(simulation_asset);
    scene::SceneDescriptionBuilder scene_builder;
    scene_builder.setWorld(identity<asset::AssetId>(10));
    scene_builder.setSimulation(identity<asset::AssetId>(11));
    {
        const auto registration = scene::worldLoadingSystemRegistration();
        const scene::WorldLoadingConfiguration config{{{0}}};
        std::vector<std::byte> bytes;
        assert(registration.configuration.encode(&config, bytes));
        assert(scene_builder.addSystem({4}, "loading", registration.type, 1,
                                       registration.description->configuration_schema_name, 1, bytes));
    }
    if (rendered)
    {
        const auto query = scene::builtinMeshQuerySystemRegistration();
        assert(scene_builder.addSystem({5}, "mesh-query", query.type, query.description->version, {}, 0));
        scene::RenderSystemConfiguration config;
        config.coordinate_page_size = 2048;
        const auto features = render::builtinRenderFeatureRegistrations();
        for (const auto name :
             {"lux.render.view_camera.v1", "lux.render.material.v1", "lux.render.mesh_stack.v1", "lux.render.light.v1",
              "lux.render.forward_mesh.v1", "lux.render.shadow_map.v1", "lux.render.mesh_shadow.v1", "lux.render.highlight.v1", "lux.render.grid3d.v1"})
        {
            if (std::string_view{name} == "lux.render.mesh_shadow.v1" && !shadows)
            {
                continue;
            }
            const auto type = render::featureId(name);
            const auto found =
                std::ranges::find(features, std::string_view{name}, [](const auto &entry) { return entry.factory.descriptor.canonical_name; });
            assert(found != features.end());
            std::vector<std::byte> defaults;
            assert(found->configuration.portable.encode_default(defaults));
            config.features.push_back({type, std::move(defaults), std::string(found->configuration.schema), found->configuration.schema_version});
        }
        const auto registration = scene::builtinRenderSystemRegistration();
        std::vector<std::byte> bytes;
        assert(registration.configuration.encode(&config, bytes));
        const auto &type = scene::RenderSystem::Description;
        assert(scene_builder.addSystem(system::SystemInstanceId{2}, "render", registration.type, type.version,
                                       type.configuration_schema_name, type.configuration_schema_version, bytes));
        assert(scene_builder.addDependency({5}, {2}));
        assert(scene_builder.bindRequirement(system::SystemInstanceId{2}, "render_runtime",
                                             std::string_view(argv[2]) == "gpu-missing-provider" ? "unregistered"
                                                                                                 : "main-window"));
    }
    auto scene_description = std::move(scene_builder).build();
    assert(scene_description);
    auto scene_asset =
        scene::SceneAsset::create(asset::AssetInfo{identity<asset::AssetId>(12)},
                                  std::make_shared<const scene::SceneDescription>(std::move(*scene_description)));
    assert(scene_asset);
    std::vector<asset::PakWriteEntry> entries;
    const auto append = [&]<class Asset>(const std::shared_ptr<Asset> &value, const char *path) {
        auto bytes = asset::TAssetSerDeser<std::remove_const_t<Asset>>::encode(
            *value, asset::AssetEncodeLimits{16 * 1024 * 1024});
        assert(bytes);
        auto owner = std::make_shared<std::vector<std::byte>>(std::move(*bytes));
        entries.push_back({value->id(), Asset::primary_magic, path, {}, cxx::SharedBytes<>::fromOwner(owner, *owner)});
    };
    append(*scene_asset, "Scene");
    append(*world_asset, "World");
    append(*simulation_asset, "Simulation");
    auto bytes = std::make_shared<std::vector<std::byte>>(std::move(*volume));
    entries.push_back({identity<asset::AssetId>(13), 1, "Storage/0", {}, cxx::SharedBytes<>::fromOwner(bytes, *bytes)});
    std::string failure;
    assert(asset::writePakFile(root / "Main.luxscene", std::move(entries), "/Scene", &failure));
    editor::ProjectManifest project{
        identity<asset::AssetId>(20),
        "Native Desktop",
        "Main.luxscene",
        {{identity<asset::AssetId>(12), editor::EProjectAssetKind::SCENE, "Main.luxscene", {}, {}, {}, "Scenes/Main"},
         {seed(10), editor::EProjectAssetKind::MODEL, "Cube.obj", "Seed.luxpak", {}, {}, "Seed"}}};
    if (std::string_view(argv[2]) == "gpu-sharing")
    {
        auto second = scene::SceneAsset::create(
            asset::AssetInfo{identity<asset::AssetId>(22)},
            std::shared_ptr<const scene::SceneDescription>(*scene_asset, &(*scene_asset)->data()));
        assert(second);
        append(*second, "Scene");
        append(*world_asset, "World");
        append(*simulation_asset, "Simulation");
        entries.push_back(
            {identity<asset::AssetId>(13), 1, "Storage/0", {}, cxx::SharedBytes<>::fromOwner(bytes, *bytes)});
        assert(asset::writePakFile(root / "Second.luxscene", std::move(entries), "/Scene", &failure));
        project.assets.push_back({identity<asset::AssetId>(22),
                                  editor::EProjectAssetKind::SCENE,
                                  "Second.luxscene",
                                  {},
                                  {},
                                  {},
                                  "Scenes/Second"});
    }
    const std::size_t unopened = argc == 5 ? std::stoul(argv[4]) : 0;
    std::size_t unopened_bytes{};
    for (std::size_t index{}; index < unopened; ++index)
    {
        std::array<std::uint8_t, 16> id_bytes{};
        id_bytes[0] = 0x72;
        id_bytes[14] = static_cast<std::uint8_t>(index >> 8);
        id_bytes[15] = static_cast<std::uint8_t>(index);
        const asset::AssetId id{id_bytes};
        const auto path = "Unopened-" + std::to_string(index) + ".luxmaterial";
        auto source = material::encodeMaterialSource({id, std::string(4096, 'x'), {}});
        assert(source);
        std::ofstream file(root / path, std::ios::binary);
        file << *source;
        assert(file.good());
        unopened_bytes += source->size();
        project.assets.push_back({id, editor::EProjectAssetKind::MATERIAL_GRAPH, path, {}, {}, {}, path});
    }
    auto encoded = editor::encodeProjectManifest(project);
    assert(encoded);
    std::ofstream manifest(root / "Project.luxproject", std::ios::binary);
    manifest << *encoded;
    assert(manifest.good());
    std::printf("native fixture: %zu objects, render=%d, page=2048\n", objects.size(), rendered);
    std::printf("unopened sources=%zu encoded_bytes=%zu manifest_bytes=%zu\n", unopened, unopened_bytes,
                encoded->size());
}
