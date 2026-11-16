#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/platform/FileIo.hpp>

#include <cassert>
#include <cstring>
#include <fstream>

#include <lux/engine/Engine.hpp>
#include <lux/engine/Init.hpp>
#include <lux/engine/SceneBuilder.hpp>
#include <lux/engine/EngineMetaFunctionRegistry.hpp>
#include <lux/engine/GeometryDeclaration.hpp>
#include <lux/engine/PartitionDeclaration.hpp>
#include <lux/engine/SceneSystemRegistration.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/core/execution/ExecutionResult.hpp>
#include <lux/engine/scene/SceneContent.hpp>
#include <lux/engine/scene/ScenePersistence.hpp>
#include <lux/engine/scene/SceneStreamingQuery.hpp>
#include <lux/engine/scene/SceneStreamingCodec.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/toolchain/scene/SceneStreamingBuild.hpp>
#include <lux/engine/resource/asset/storage/directory/DirectoryAssetProvider.hpp>
#include <lux/engine/world/World.hpp>
#include <lux/engine/world/ObserverComponent.hpp>
#include <lux/engine/world/LifecyclePolicyComponent.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/ecs/HierarchySchema.hpp>
#include <lux/engine/ecs/Parent.hpp>
#if defined(LUX_ENGINE_WITH_RENDER)
#include <lux/engine/systems/render/mesh/MeshVisibilitySystem.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/systems/render/view/RenderTargetComponent.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/ecs/VisualSchema.hpp>
#include <lux/engine/ecs/VisibilityComponent.hpp>
#include <lux/engine/ecs/RenderableMeshComponent.hpp>
#include <lux/engine/scene/ViewComponent.hpp>
#include <lux/engine/world/WorldSpatialIndex.hpp>
#endif

namespace
{
    using namespace lux;

    template <class Sender> auto wait(execution::ExecutionRuntime& execution, Sender sender)
    {
        std::optional<typename execution::detail::ExecutionTypes<Sender>::Result> result;
        auto accepted = execution.spawn(stdexec::then(
            execution::asResult(stdexec::starts_on(execution.mainScheduler(), std::move(sender))),
            [&](auto completed) noexcept { result.emplace(std::move(completed)); }
        ));
        assert(accepted);
        assert(execution.waitUntil([&]() noexcept { return result.has_value(); }));
        return std::move(*result);
    }

#if defined(LUX_ENGINE_WITH_RENDER)
    rdesc::Mesh visibilityMesh()
    {
        rdesc::Mesh mesh;
        mesh.vertices.resize(3);
        mesh.indices = {0, 1, 2};
        const std::array positions{Eigen::Vector3f{-1, -1, 0}, Eigen::Vector3f{1, -1, 0}, Eigen::Vector3f{0, 1, 0}};
        for (std::size_t i = 0; i < 3; ++i)
        {
            auto& vertex = mesh.vertices[i];
            vertex.position = positions[i];
            vertex.normal = {0, 0, 1};
            vertex.tangent = {1, 0, 0};
            vertex.bitangent = {0, 1, 0};
            vertex.uv = {0, 0};
            std::fill(std::begin(vertex.bone.bone_ids), std::end(vertex.bone.bone_ids), -1);
            std::fill(std::begin(vertex.bone.weights), std::end(vertex.bone.weights), 0.0f);
        }
        return mesh;
    }

    void visibility(engine::EngineContext& context, asset::AssetId mesh_id)
    {
        namespace rendering = engine::render;
        using Requirement = ecs::EVisibilityRequirement;
        // The generated author codec retains permission but excludes the algorithm's current result.
        {
            world::World source{world::WorldDescription{"Visibility author codec", {}}};
            const auto entity = source.registry().create();
            const world::WorldObjectId identity{*uuids::uuid::from_string("629c02e8-504e-4ebe-b633-ce8fc2269954")};
            source.registry().emplace<world::WorldObjectId>(entity, identity);
            source.registry().emplace<ecs::VisibilityComponent>(entity, false, Requirement::CANDIDATE);
            const auto definitions = ecs::visualComponentSchemas();
            auto schemas = ecs::ComponentSchemaSet::build({definitions.begin(), definitions.end()});
            assert(schemas);
            std::vector<scene::SceneObjectData> captured;
            assert(scene::captureSceneBlock(source, std::span{&entity, 1}, *schemas, captured));
            world::World restored{world::WorldDescription{"Restored permission", {}}};
            assert(scene::restoreSceneGroup(restored, captured, *schemas));
            const auto copy = restored.identities().entity(identity);
            assert(copy != ecs::NullEntity);
            const auto& value = restored.registry().get<ecs::VisibilityComponent>(copy);
            assert(!value.is_visible && value.global_requirement == Requirement::NONE);
            const auto& original = source.registry().get<ecs::VisibilityComponent>(entity);
            assert(original.global_requirement == Requirement::CANDIDATE);
        }
        engine::Engine engine{context};
        auto world = std::make_unique<world::World>(world::WorldDescription{"Visibility", {{"fixture.mesh", {}}}});
        auto& registry = world->registry();
        auto visibility = rendering::MeshVisibilitySystem::create(*world, context, 0, 10, 14, 8, 1);
        assert(visibility);
        auto& index = world->spatialIndex(0);
        const auto mesh = registry.create();
        const auto outer = registry.create();
        for (auto entity : {mesh, outer})
        {
            registry.emplace<ecs::Transform3D>(entity);
            ecs::Mesh3D geometry;
            geometry.value.mesh = mesh_id;
            registry.emplace<ecs::Mesh3D>(entity, geometry);
            registry.emplace<ecs::RenderableMeshComponent>(entity);
        }
        registry.emplace<ecs::VisibilityComponent>(outer, false);
        registry.patch<ecs::Transform3D>(outer, [](auto& value) noexcept { value.translation.x() = 12; });
        const auto first_view = registry.create();
        const auto second_view = registry.create();
        for (auto entity : {first_view, second_view})
        {
            registry.emplace<scene::ViewComponent>(entity);
            registry.emplace<ecs::Transform3D>(entity);
            rendering::RenderTargetComponent target;
            target.description.width = 640;
            target.description.height = 480;
            registry.emplace<rendering::RenderTargetComponent>(entity, target);
        }
        registry.patch<ecs::Transform3D>(second_view, [](auto& value) noexcept { value.translation.x() = 100; });
        auto candidate = std::make_unique<scene::SceneInstance>(std::move(world), std::make_unique<simulation::Simulation>());
        candidate->add(std::move(*visibility));
        auto scene_id = engine.addScene(candidate, true);
        assert(scene_id && wait(context.execution(), engine.whenSceneInitialized(*scene_id)));
        auto reference = context.assets().acquire<asset::MeshAsset>(mesh_id);
        assert(reference);
        auto replacement = visibilityMesh();
        replacement.vertices[0].position.x() = -2;
        replacement.vertices[1].position.x() = 2;
        bool written = false;
        unsigned mesh_updates = 0;
        auto author_updated = [&](ecs::Registry&, ecs::Entity) noexcept { ++mesh_updates; };
        entt::scoped_connection connection = registry.on_update<ecs::Mesh3D>()
            .connect<&decltype(author_updated)::operator()>(author_updated);
        unsigned visibility_events = 0, after_user_hide = 0;
        auto visibility_updated = [&](ecs::Registry&, ecs::Entity) noexcept { ++visibility_events; };
        entt::scoped_connection visibility_connection = registry.on_update<ecs::VisibilityComponent>()
            .connect<&decltype(visibility_updated)::operator()>(visibility_updated);
        unsigned phase = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const auto requirement = [&](ecs::Entity entity) noexcept
        {
            const auto* value = registry.valid(entity) ? registry.try_get<ecs::VisibilityComponent>(entity) : nullptr;
            return value && value->is_visible ? value->global_requirement : Requirement::NONE;
        };
        engine.addEngineStageCallback([&]() noexcept -> engine::Engine::Result
        {
            assert(std::chrono::steady_clock::now() < deadline);
            if (phase == 0 && !index.actualBounds(mesh))
                registry.patch<ecs::Transform3D>(mesh, [](auto&) noexcept {});
            if (phase == 0 && index.actualBounds(outer) && requirement(mesh) == Requirement::CANDIDATE)
            {
                assert(requirement(outer) == Requirement::NONE); // Retain range must not start first-time preparation.
                assert(!registry.get<ecs::VisibilityComponent>(outer).is_visible); // Preserve pre-existing permission.
                assert(index.actualBounds(mesh)->minimum.z < 0 && index.actualBounds(mesh)->maximum.z > 0);
                registry.patch<ecs::Transform3D>(mesh, [](auto& value) noexcept { value.translation.x() = 12; });
                phase = 1;
            }
            else if (phase == 1 && requirement(mesh) == Requirement::RETAIN)
            {
                registry.patch<ecs::Transform3D>(mesh, [](auto& value) noexcept { value.translation.x() = 30; });
                phase = 2;
            }
            else if (phase == 2 && requirement(mesh) == Requirement::NONE)
            {
                registry.patch<ecs::Transform3D>(first_view, [](auto& value) noexcept { value.translation.x() = 30; });
                registry.patch<ecs::Transform3D>(second_view, [](auto& value) noexcept { value.translation.x() = 30; });
                phase = 3;
            }
            else if (phase == 3 && requirement(mesh) == Requirement::CANDIDATE)
            {
                // A late consumer reads current component state without replaying publication history.
                assert(registry.get<ecs::VisibilityComponent>(mesh).global_requirement == Requirement::CANDIDATE);
                registry.remove<rendering::RenderTargetComponent>(first_view);
                phase = 4;
            }
            else if (phase == 4)
            {
                assert(requirement(mesh) == Requirement::CANDIDATE);
                registry.patch<ecs::VisibilityComponent>(mesh, [](auto& value) noexcept { value.is_visible = false; });
                after_user_hide = visibility_events;
                phase = 5;
            }
            else if (phase == 5 && requirement(mesh) == Requirement::NONE)
            {
                assert(registry.get<ecs::VisibilityComponent>(mesh).global_requirement == Requirement::CANDIDATE);
                assert(visibility_events == after_user_hide); // No feedback patch of unchanged global state.
                registry.patch<ecs::VisibilityComponent>(mesh, [](auto& value) noexcept { value.is_visible = true; });
                phase = 6;
            }
            else if (phase == 6 && requirement(mesh) == Requirement::CANDIDATE)
            {
                registry.patch<rendering::RenderTargetComponent>(second_view, [](auto& value) noexcept
                { value.description.width = 0; });
                registry.patch<ecs::Transform3D>(second_view, [](auto& value) noexcept { value.translation.x() = 1000; });
                phase = 7;
            }
            else if (phase == 7)
            {
                assert(requirement(mesh) == Requirement::CANDIDATE); // Suspended Target preserves its last range.
                registry.patch<rendering::RenderTargetComponent>(second_view, [](auto& value) noexcept
                { value.description.width = 640; });
                phase = 8;
            }
            else if (phase == 8 && requirement(mesh) == Requirement::NONE)
            {
                registry.patch<ecs::Transform3D>(second_view, [](auto& value) noexcept { value.translation.x() = 30; });
                phase = 9;
            }
            else if (phase == 9 && requirement(mesh) == Requirement::CANDIDATE)
            {
                assert(context.execution().spawn(stdexec::then(execution::asResult(context.assets().write(*reference, replacement)),
                    [&](auto result) noexcept { assert(result); written = true; })));
                phase = 10;
            }
            else if (phase == 10 && written && index.actualBounds(mesh) && index.actualBounds(mesh)->maximum.x == 32)
            {
                assert(mesh_updates == 0);
                registry.patch<ecs::Transform3D>(mesh, [](auto& value) noexcept { value.scale.x() = -2; });
                phase = 11;
            }
            else if (phase == 11 && index.actualBounds(mesh) && index.actualBounds(mesh)->maximum.x == 34)
            {
                assert(index.actualBounds(mesh)->minimum.x == 26);
                registry.patch<ecs::Transform3D>(mesh, [](auto& value) noexcept
                {
                    value.rotation = Eigen::AngleAxisd(1.5707963267948966, Eigen::Vector3d::UnitZ());
                    value.scale = {2, 3, 1};
                });
                phase = 12;
            }
            else if (phase == 12 && index.actualBounds(mesh) &&
                std::abs(index.actualBounds(mesh)->maximum.y - 4) < 1e-10)
            {
                const auto& bounds = *index.actualBounds(mesh);
                assert(std::abs(bounds.minimum.x - 27) < 1e-10 && std::abs(bounds.maximum.x - 33) < 1e-10);
                registry.destroy(mesh);
                phase = 13;
            }
            else if (phase == 13 && !index.actualBounds(mesh))
            {
                assert(requirement(mesh) == Requirement::NONE);
                phase = 14;
                engine.requestStop();
            }
            return {};
        }, engine::EEngineStage::END_UPDATE);
        assert(engine.run() && phase == 14);
        visibility_connection.release();
        connection.release(); // These observers borrow the Registry that removeScene destroys.
        assert(wait(context.execution(), engine.removeScene(*scene_id)));
        assert(wait(context.execution(), context.assets().write(*reference, replacement))); // No retired receiver remains.
    }
#endif

    void persistenceAndLoading(const std::filesystem::path& directory)
    {
        constexpr std::size_t limit = 4 * 1024 * 1024;
        const auto root = directory / "streaming";
        std::filesystem::remove_all(root); // This test owns only its dedicated child directory.
        std::filesystem::create_directories(root);
#if defined(LUX_ENGINE_WITH_RENDER)
        const asset::AssetId mesh_id{*uuids::uuid::from_string("629c02e8-504e-4ebe-b633-ce8fc2269901")};
        const auto mesh = visibilityMesh();
        const auto encoded_mesh = asset::TAssetSerDeser<asset::MeshAsset>::encode(
            {.id = mesh_id, .type = asset::MeshAsset::asset_type}, mesh, {}, asset::AssetEncodeLimits{limit}
        );
        assert(encoded_mesh && platform::writeFileAtomic(root / "mesh.luxasset", *encoded_mesh, platform::EFileWriteMode::CREATE));
#endif
        auto provider = asset::DirectoryAssetProvider::create(root);
        assert(provider);
        asset::MountLease mount;
        std::vector<std::unique_ptr<asset::AssetCodec>> codecs;
        codecs.push_back(scene::createSceneCodec());
#if defined(LUX_ENGINE_WITH_RENDER)
        render::RenderServerConfig rendering;
        rendering.channel_name = "lux.scene.persistence";
        auto created = engine::init(
            {.cpu_concurrency = 2, .timer = {16}, .blocking = execution::BlockingSchedulerConfig{1, 32}},
            {limit, limit, 32}, std::move(rendering), {}, {}, std::move(codecs)
        );
#else
        auto created = engine::init(
            {.cpu_concurrency = 2, .timer = {16}, .blocking = execution::BlockingSchedulerConfig{1, 32}},
            {limit, limit, 32}, {}, std::move(codecs)
        );
#endif
        assert(created);
        auto context = std::move(*created);
        auto mounted = context->assetVfs().mount({"/Scenes", *provider});
        assert(mounted);
        mount = std::move(*mounted);
        auto& assets = context->assets();
        auto& execution = context->execution();
        uuids::uuid_name_generator ids{*uuids::uuid::from_string("629c02e8-504e-4ebe-b633-ce8fc2269954")};
        const world::WorldObjectId first{ids("object.first")}, second{ids("object.second")};
        {
            world::World source{world::WorldDescription{"Capture", {}}};
            auto& registry = source.registry();
            const auto parent = registry.create();
            const auto child = registry.create();
            registry.emplace<world::WorldObjectId>(parent, first);
            registry.emplace<world::WorldObjectId>(child, second);
            registry.emplace<ecs::Parent>(child, parent);
            const auto hierarchy = ecs::hierarchyComponentSchemas();
            auto schemas = ecs::ComponentSchemaSet::build({hierarchy.begin(), hierarchy.end()});
            assert(schemas);
            std::vector<scene::SceneObjectData> captured{{first, {}}};
            assert(!scene::captureSceneBlock(source, std::span{&child, 1}, *schemas, captured));
            assert(captured.size() == 1 && captured.front().id == first); // Failure preserves output.
            registry.patch<ecs::Parent>(child, [](auto& value) noexcept { value.entity = ecs::NullEntity; });
            assert(scene::captureSceneBlock(source, std::span{&child, 1}, *schemas, captured));
            world::World restored{world::WorldDescription{"Restore", {}}};
            assert(scene::restoreSceneGroup(restored, captured, *schemas));
            assert(captured.size() == 1 && captured.front().components.size() == 1);
            const auto current = captured.front().components.front();
            const auto reject = [&](scene::SceneComponentData field)
            {
                world::World candidate{world::WorldDescription{"Reject unsupported schema", {}}};
                const std::array objects{
                    scene::SceneObjectData{first, {}},
                    scene::SceneObjectData{second, {std::move(field)}}
                };
                assert(!scene::restoreSceneGroup(candidate, objects, *schemas));
                // Even the valid first object must not be partially adopted.
                assert(candidate.identities().size() == 0);
                assert(candidate.registry().view<world::WorldObjectId>().empty());
            };
            auto unsupported = current;
            unsupported.version = 0;
            reject(unsupported);
            unsupported.version = current.version + 1;
            reject(unsupported);
            unsupported = current;
            unsupported.schema = ecs::componentSchemaId("test.removed.Parent");
            reject(unsupported);
            unsupported.schema = ecs::componentSchemaId("test.unknown.Component");
            reject(unsupported);
        }
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
        for (const auto& schema : world::worldComponentSchemas()) assert(metadata.addComponent(schema));
        scene::SceneDescription description;
        description.world.name = "Streamed";
        world::SceneGridConfiguration grid;
        grid.cell_size = 100;
        const auto grid_codec = serialization::makePortableValueCodec<world::SceneGridConfiguration>();
        std::vector<std::byte> grid_bytes;
        assert(grid_codec.encode(&grid, grid_bytes));
        description.world.supported_geometries.push_back({"fixture.mesh", {"uniform", 1, grid_bytes}});
        std::vector<scene::SceneBlockUpdate> updates{
            {0, {{first, {}}}, {{first, {{0, {{0, 0, 0}, {1, 1, 1}}}}}}},
            {1, {{second, {}}}, {{second, {{0, {{500, 0, 0}, {501, 1, 1}}}}}}}
        };
        auto streaming = wait(execution, engine::toolchain::buildSceneStreaming(
            assets, {}, description, updates, metadata, limit
        ));
        assert(streaming && streaming->groups.size() == 2);
        std::vector<std::byte> nested;
        serialization::BinaryWriter nested_writer(nested);
        auto budget = limit;
        assert(nested_writer.writeUnsigned(std::uint32_t{0x1234}));
        assert(scene::writeSceneStreaming(nested_writer, *streaming, budget));
        assert(nested_writer.writeUnsigned(std::uint32_t{0x5678}));
        serialization::BinaryReader nested_reader(nested);
        assert(nested_reader.readUnsigned<std::uint32_t>().value() == 0x1234);
        budget = limit;
        auto decoded_directory = scene::readSceneStreaming(nested_reader, budget);
        assert(decoded_directory && decoded_directory->input_digest == streaming->input_digest);
        assert(nested_reader.readUnsigned<std::uint32_t>().value() == 0x5678 && !nested_reader.remaining());
        const auto previous_size = nested.size();
        budget = 63;
        auto bounded = scene::writeSceneStreaming(nested_writer, *streaming, budget);
        assert(!bounded && bounded.error().type == error::errorId("lux.scene.content.capacity"));
        assert(nested.size() == previous_size);
        auto invalid_directory = *streaming;
        invalid_directory.block_groups[0] = UINT32_MAX;
        budget = limit;
        assert(!scene::writeSceneStreaming(nested_writer, invalid_directory, budget));
        assert(nested.size() == previous_size);
        const asset::AssetInfo info{.id = asset::AssetId{ids("streamed")}, .type = scene::SceneAsset::asset_type};
        auto asset = wait(execution, scene::createSceneAsset(
            assets, "/Scenes/world", info, description, updates, *streaming
        ));
        assert(asset && assets.resolve("/Scenes/world") == info.id);
        auto old = wait(execution, assets.read(*asset));
        assert(old && (*old)->streaming.groups.size() == 2);
        const auto& a = (*old)->blocks[0];
        auto bytes = wait(execution, assets.readRange(*old, a.offset, a.size));
        assert(bytes && scene::decodeSceneBlock(bytes->view(), a, limit)->front().id == first);
        const auto physical_root = wait(execution, stdexec::then(
            stdexec::schedule(*execution.blockingIoScheduler()),
            [&]() noexcept { return (*provider)->open(info.id, limit); }
        ));
        assert(physical_root);
        const auto revision = assets.revision(*asset);
        std::uint64_t abandoned_offset = 0;
        auto unpublished = wait(execution, assets.withContentWriter(
            *asset, *old, asset::EAssetContentWrite::APPEND,
            [&](asset::AssetContentWriter& writer) noexcept -> exec::task<engine::assets::AssetResult<void>>
            {
                auto appended = co_await stdexec::continues_on(stdexec::then(
                    stdexec::schedule(*execution.blockingIoScheduler()),
                    [&]() noexcept { return writer.append(std::array<std::byte, 16>{}); }
                ), execution.mainScheduler());
                assert(appended);
                abandoned_offset = *appended;
                co_return cxx::unexpected(error::Error{engine::assets::kAssetWriteFailure});
            }
        ));
        assert(!unpublished && abandoned_offset != 0 && assets.revision(*asset) == revision);
        assert(assets.isCurrent(*old));
        const auto preserved_root = wait(execution, stdexec::then(
            stdexec::schedule(*execution.blockingIoScheduler()),
            [&]() noexcept { return (*provider)->open(info.id, limit); }
        ));
        assert(preserved_root && std::ranges::equal(physical_root->bytes.view(), preserved_root->bytes.view()));
        std::vector<std::uint32_t> queried;
        assert(scene::querySceneGroups((*old)->streaming, 0, {{-10, -10, -10}, {10, 10, 10}}, queried));
        assert(queried == std::vector<std::uint32_t>{0});
        auto wrong = **old;
        wrong.streaming.input_digest = cxx::algorithm::Sha256::hash("wrong");
        assert(!asset::TAssetSerDeser<scene::SceneAsset>::encode(info, wrong, {}, asset::AssetEncodeLimits{limit}));
        const auto unchanged = (*old)->blocks[1].offset;
        auto change = updates[0];
        change.summaries[0].geometry[0].bounds.maximum.x = 2;
        auto amended = wait(execution, engine::toolchain::buildSceneStreaming(
            assets, *old, description, std::span{&change, 1}, metadata, limit
        ));
        assert(amended);
        assert(wait(execution, scene::saveScene(assets, *asset, *old, description, {change}, *amended)));
        auto saved = wait(execution, assets.read(*asset));
        assert(saved && (*saved)->blocks[1].offset == unchanged && (*saved)->blocks[0].offset > a.offset);
        assert((*saved)->blocks[0].offset >= abandoned_offset + 16); // Unreferenced APPEND tails are not truncated.
        assert(!assets.isCurrent(*old) && assets.isCurrent(*saved));
        assert(!wait(execution, scene::saveScene(assets, *asset, *old, description, {change}, *amended)));
        auto old_bytes = wait(execution, assets.readRange(*old, a.offset, a.size));
        assert(old_bytes && std::ranges::equal(old_bytes->view(), bytes->view()));
        assert(wait(execution, scene::compactSceneAsset(assets, *asset, *saved, 64)));
        auto compacted = wait(execution, assets.read(*asset));
        assert(compacted && (*compacted)->streaming.input_digest == (*saved)->streaming.input_digest);
        auto retained_bytes = wait(execution, assets.readRange(*old, a.offset, a.size));
        assert(retained_bytes && std::ranges::equal(retained_bytes->view(), bytes->view()));
        const asset::AssetInfo copy_info{.id = asset::AssetId{ids("copy")}, .type = scene::SceneAsset::asset_type};
        auto copy = wait(execution, scene::copySceneAsset(assets, *compacted, "/Scenes/copy", copy_info));
        assert(copy);
        auto copied = wait(execution, assets.read(*copy));
        assert(copied && (*copied)->streaming.input_digest == (*compacted)->streaming.input_digest);
        scene::SceneBlockUpdate erased{0, {}, {}};
        auto deleted_directory = wait(execution, engine::toolchain::buildSceneStreaming(
            assets, *compacted, description, std::span{&erased, 1}, metadata, limit
        ));
        assert(deleted_directory && deleted_directory->block_groups[0] == UINT32_MAX);
        auto deletion = wait(execution, scene::saveScene(
            assets, *asset, *compacted, description, {erased}, *deleted_directory
        ));
        assert(deletion);
        auto deleted = wait(execution, assets.read(*asset));
        assert(deleted && (*deleted)->blocks.size() == 2 && !(*deleted)->blocks[0].size && (*deleted)->blocks[1].size);
        assert(!wait(execution, engine::toolchain::buildSceneStreaming(
            assets, *deleted, description, std::span{&change, 1}, metadata, limit
        ))); // Deleted block ordinals cannot be recycled.
        auto cross_owner = description;
        cross_owner.world.supported_geometries.push_back({"fixture.other", {"uniform", 1, grid_bytes}});
        auto invalid_updates = updates;
        invalid_updates[1].summaries[0].geometry[0].geometry = 1;
        auto independent = wait(execution, engine::toolchain::buildSceneStreaming(
            assets, {}, cross_owner, invalid_updates, metadata, limit
        ));
        assert(independent && independent->groups.size() == 2);
        assert(independent->block_groups[0] != independent->block_groups[1]);
        // A single block must still have one geometric loading owner.
        invalid_updates[0].summaries[0].geometry.push_back({1, {{0, 0, 0}, {1, 1, 1}}});
        assert(!wait(execution, engine::toolchain::buildSceneStreaming(
            assets, {}, cross_owner, invalid_updates, metadata, limit
        )));
        auto registration = scene::worldLoadingSystemRegistration();
        const auto loader_name = registration.name;
        const auto loader_version = registration.configuration_version;
        assert(metadata.addSceneSystem(std::move(registration)));
        const scene::WorldLoadingLimits loading_limits{4, 64, 100000, limit, limit, limit};
        auto loading_bytes = scene::makeWorldLoadingSystemConfiguration(
            copy_info.id, "fixture.mesh", false, loading_limits
        );
        assert(loading_bytes);
        auto runtime_description = description;
        runtime_description.systems.push_back({loader_name, "world", loader_version, std::move(*loading_bytes)});
        {
            engine::Engine engine{*context};
            engine::Engine::SceneBuilder builder{engine, metadata};
            auto invalid = runtime_description;
            invalid.world.supported_geometries[0].name = "removed.geometry";
            assert(!builder.build(invalid));
            invalid = runtime_description;
            invalid.world.supported_geometries[0].partitioner.name = "removed.partitioner";
            assert(!builder.build(invalid));
            invalid = runtime_description;
            ++invalid.world.supported_geometries[0].partitioner.configuration_version;
            assert(!builder.build(invalid));
            invalid = runtime_description;
            invalid.systems[0].type = "removed.system";
            assert(!builder.build(invalid));
            invalid = runtime_description;
            ++invalid.systems[0].configuration_version;
            assert(!builder.build(invalid));
            invalid = runtime_description;
            invalid.systems[0].configuration.pop_back();
            assert(!builder.build(invalid));
            auto unknown_geometry = scene::makeWorldLoadingSystemConfiguration(
                copy_info.id, "missing.geometry", false, loading_limits
            );
            assert(unknown_geometry);
            invalid.systems[0].configuration = std::move(*unknown_geometry);
            assert(!builder.build(invalid));
        }
        {
            // A separately assembled loader for non-geometric content needs no fake geometry or Observer.
            scene::SceneDescription unindexed;
            unindexed.world.name = "Unindexed";
            auto unindexed_updates = updates;
            for (auto& update : unindexed_updates) update.summaries[0].geometry.clear();
            auto unindexed_directory = wait(execution, engine::toolchain::buildSceneStreaming(
                assets, {}, unindexed, unindexed_updates, metadata, limit
            ));
            assert(unindexed_directory && !unindexed_directory->groups[0].geometry);
            const asset::AssetInfo unindexed_info{
                .id = asset::AssetId{ids("unindexed")}, .type = scene::SceneAsset::asset_type
            };
            auto unindexed_asset = wait(execution, scene::createSceneAsset(
                assets, "/Scenes/unindexed", unindexed_info, unindexed, unindexed_updates, *unindexed_directory
            ));
            assert(unindexed_asset);
            auto configuration = scene::makeWorldLoadingSystemConfiguration(
                unindexed_info.id, std::nullopt, true, loading_limits
            );
            assert(configuration);
            unindexed.systems.push_back({loader_name, "world", loader_version, std::move(*configuration)});
            engine::Engine engine{*context};
            engine::Engine::SceneBuilder builder{engine, metadata};
            auto built = builder.build(unindexed);
            assert(built);
            auto& world = (*built)->world();
            auto scene_id = engine.addScene(*built, true);
            assert(scene_id && wait(execution, engine.whenSceneInitialized(*scene_id)));
            assert(world.identities().entity(first) != ecs::NullEntity);
            assert(world.identities().entity(second) != ecs::NullEntity);
            assert(wait(execution, engine.removeScene(*scene_id)));
        }
        {
            engine::Engine engine{*context};
            engine::Engine::SceneBuilder builder{engine, metadata};
            auto invalid = runtime_description;
            invalid.systems[0].configuration_version = 2;
            assert(!builder.build(invalid));
            auto built = builder.build(runtime_description);
            assert(built);
            auto& world = (*built)->world();
            auto& registry = world.registry();
            const auto observer1 = registry.create();
            const auto observer2 = registry.create();
            registry.emplace<ecs::Transform3D>(observer1);
            registry.emplace<ecs::Transform3D>(observer2);
            registry.emplace<world::ObserverComponent>(observer1, 10.0);
            registry.emplace<world::ObserverComponent>(observer2, 10.0);
            auto scene_id = engine.addScene(*built, true);
            assert(scene_id && wait(execution, engine.whenSceneInitialized(*scene_id)));
            unsigned phase = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            engine.addEngineStageCallback([&]() noexcept -> engine::Engine::Result
            {
                assert(std::chrono::steady_clock::now() < deadline);
                const bool loaded = world.identities().entity(first) != ecs::NullEntity;
                assert(world.identities().entity(second) == ecs::NullEntity);
                if (phase == 0 && loaded)
                {
                    registry.emplace<world::LifecyclePolicyComponent>(
                        world.identities().entity(first), world::ELifecyclePolicy::MANUAL
                    );
                    registry.remove<world::ObserverComponent>(observer1);
                    phase = 1;
                }
                else if (phase == 1)
                {
                    assert(loaded);
                    registry.remove<world::ObserverComponent>(observer2);
                    phase = 2;
                }
                else if (phase == 2)
                {
                    assert(loaded);
                    registry.patch<world::LifecyclePolicyComponent>(
                        world.identities().entity(first),
                        [](auto& value) noexcept { value.policy = world::ELifecyclePolicy::AUTOMATIC; }
                    );
                    phase = 3;
                }
                else if (phase == 3 && !loaded)
                {
                    phase = 4;
                    engine.requestStop();
                }
                return {};
            }, engine::EEngineStage::END_UPDATE);
            assert(engine.run() && phase == 4);
        }
        {
            engine::Engine engine{*context};
            engine::Engine::SceneBuilder builder{engine, metadata};
            auto built = builder.build(runtime_description);
            assert(built);
            auto scene_id = engine.addScene(*built, true);
            assert(scene_id && wait(execution, engine.removeScene(*scene_id)));
        }
#if defined(LUX_ENGINE_WITH_RENDER)
        visibility(*context, mesh_id);
#endif
        // All references above are destroyed before Context; its repository retains immutable old files safely.
    }
}

int main(int argc, char** argv)
{
    using namespace lux;
    assert(argc == 2);
    const auto directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    const auto file = directory / "generic.scene";
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("629c02e8-504e-4ebe-b633-ce8fc2269954")};
    const asset::AssetInfo info{.id = asset::AssetId{ids("scene")}, .type = scene::SceneAsset::asset_type};
    auto data = std::make_shared<scene::SceneData>();
    data->description.world.name = "Generic";
    data->content_file = "generic.content";
    data->description.simulation.groups.push_back({std::chrono::milliseconds(16), {{"physics", "fixed", 0, {}}}});
    data->description.simulation.groups.push_back({{}, {{"logic", "frame", 0, {}}}});
    data->streaming.input_digest = scene::sceneStreamingInputDigest(*data).value();
    auto asset = scene::SceneAsset::create(info, data);
    assert(asset);
    constexpr std::size_t limit = 1024 * 1024;
    auto codec = scene::createSceneCodec();
    auto encoded = asset::TAssetSerDeser<scene::SceneAsset>::encode(**asset, asset::AssetEncodeLimits{limit});
    assert(encoded);
    auto borrowed = asset::TAssetSerDeser<scene::SceneAsset>::encode(info, *data, {}, asset::AssetEncodeLimits{limit});
    auto generic = codec->encode(info, data.get(), {}, asset::AssetEncodeLimits{limit});
    assert(borrowed && generic && *borrowed == *encoded && *generic == *encoded);
    auto wrong = info;
    wrong.type = {};
    assert(!asset::TAssetSerDeser<scene::SceneAsset>::encode(wrong, *data, {}, asset::AssetEncodeLimits{limit}));
    assert(!asset::TAssetSerDeser<scene::SceneAsset>::encode(info, *data, {}, asset::AssetEncodeLimits{1}));
    std::filesystem::remove(file);
    assert(platform::writeFileAtomic(file, *encoded, platform::EFileWriteMode::CREATE));
    const auto duplicate = platform::writeFileAtomic(file, *encoded, platform::EFileWriteMode::CREATE);
    assert(!duplicate && duplicate.error().code == platform::EFileIoError::DESTINATION_EXISTS);
    const auto read = platform::readFileBounded(file, limit);
    assert(read && *read == *encoded);
    const auto decode = [&](const std::vector<std::byte>& bytes)
    {
        auto backing = std::make_shared<const std::vector<std::byte>>(bytes);
        return asset::TAssetSerDeser<scene::SceneAsset>::decode(
            info.id, cxx::SharedBytes<>::fromOwner(backing, *backing), {limit, limit, 8});
    };
    const auto decoded = decode(*read);
    assert(decoded);
    const auto image_owner = std::make_shared<const std::vector<std::byte>>(*encoded);
    const auto image = asset::inspectCookedAssetImage(
        cxx::SharedBytes<>::fromOwner(image_owner, *image_owner), {limit, limit, 8}
    );
    assert(image);
    for (const std::uint32_t unsupported_version : {20250128u, asset::kCookedAssetVersionV2 + 1})
    {
        auto bytes = *encoded;
        std::memcpy(bytes.data() + sizeof(std::uint32_t), &unsupported_version, sizeof(unsupported_version));
        const auto owner = std::make_shared<const std::vector<std::byte>>(bytes);
        const auto rejected = asset::inspectCookedAssetImage(
            cxx::SharedBytes<>::fromOwner(owner, *owner), {limit, limit, 8}
        );
        assert(!rejected && rejected.error().code == asset::EAssetDecodeError::UNSUPPORTED_VERSION);
        asset::CookedAssetMetadata metadata;
        std::uint32_t magic{};
        const auto header = asset::inspectCookedAssetHeader(bytes, bytes.size(), metadata, magic);
        assert(!header && header.error().code == asset::EAssetDecodeError::UNSUPPORTED_VERSION);
    }
    const auto payload_offset = image->data().view().data() - image_owner->data();
    for (const std::uint32_t unsupported_version : {2u, 4u})
    {
        auto unsupported = *encoded;
        std::vector<std::byte> version;
        serialization::BinaryWriter writer{version};
        assert(writer.writeUnsigned(unsupported_version));
        std::copy(version.begin(), version.end(), unsupported.begin() + payload_offset);
        assert(!decode(unsupported)); // Current Scene root is version 3, with no fallback decoder.
    }
    assert((*decoded)->data().description.simulation.groups[0].fixed_step->count() == 16000000);
    assert(!(*decoded)->data().description.simulation.groups[1].fixed_step);
    const auto reencoded = asset::TAssetSerDeser<scene::SceneAsset>::encode(**decoded, asset::AssetEncodeLimits{limit});
    assert(reencoded && *reencoded == *encoded);
    const auto size_limited = platform::readFileBounded(file, 1);
    assert(!size_limited && size_limited.error().code == platform::EFileIoError::LIMIT);
    assert(!asset::TAssetSerDeser<scene::SceneAsset>::encode(**asset, asset::AssetEncodeLimits{1}));
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = platform::writeFileAtomic(
        file, *encoded, platform::EFileWriteMode::REPLACE, stop.get_token()
    );
    assert(!cancelled && cancelled.error().code == platform::EFileIoError::CANCELLED);
    assert(platform::readFileBounded(file, limit).value() == *encoded);
    const auto read_cancelled = platform::readFileBounded(file, limit, stop.get_token());
    assert(!read_cancelled && read_cancelled.error().code == platform::EFileIoError::CANCELLED);
    const auto missing = platform::readFileBounded(directory / "missing.scene", limit);
    assert(!missing && missing.error().code == platform::EFileIoError::IO);
    {
        std::ofstream corrupt(file, std::ios::binary);
        corrupt << "broken package";
    }
    const auto broken = platform::readFileBounded(file, limit);
    assert(broken && !decode(*broken));
    assert(platform::writeFileAtomic(file, *encoded, platform::EFileWriteMode::REPLACE));
    assert(decode(platform::readFileBounded(file, limit).value()));
    persistenceAndLoading(directory);
}
