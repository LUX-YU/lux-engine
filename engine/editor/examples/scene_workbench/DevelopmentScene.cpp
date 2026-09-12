#include "DevelopmentScene.hpp"
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/simulation/TransformSystem.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/function/render/client/core/RenderFeatureMetaModule.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <array>
#include <memory>
#include <utility>
#include <vector>
#include <atomic>
#include <functional>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <thread>
namespace lux::editor::examples
{
    namespace
    {
        template <class Type> [[nodiscard]] Type uuidId(std::uint8_t tail)
        {
            std::array<std::uint8_t, 16U> bytes{};
            bytes.back() = tail;
            return Type{uuids::uuid(bytes)};
        }

        [[nodiscard]] asset::AssetId assetId(std::uint8_t tail)
        {
            std::array<std::uint8_t, 16U> bytes{};
            bytes.back() = tail;
            return asset::AssetId{bytes};
        }

        [[nodiscard]] lux::cxx::expected<std::unique_ptr<scene::Scene>, EDemoBuildError> buildDevelopmentScene(
            const scene::SceneMetaManager &meta, scene::RenderRuntime &runtime, double coordinate_page_size,
            bool shared_shadows)
        {
            world::WorldDescriptionBuilder world_builder;
            if (!world_builder.setIdentity(uuidId<world::WorldBundleId>(1U), uuidId<world::WorldBundleGeneration>(1U),
                                           "lux-editor-development-scene") ||
                !world_builder.setPartitioner({world::worldPartitionerId("lux.editor.none"), 1U}, 0U))
            {
                return lux::cxx::unexpected(EDemoBuildError::WORLD_BUILD_FAILURE);
            }
            auto world = std::move(world_builder).build();
            if (!world)
                return lux::cxx::unexpected(EDemoBuildError::WORLD_BUILD_FAILURE);

            simulation::SimulationDescriptionBuilder simulation_builder;
            auto transform_config = simulation::makeTransformSystemConfiguration(1024, {2048, 1024 * 1024});
            if (!transform_config ||
                !simulation_builder.addSystem(system::SystemInstanceId{1}, "transform",
                                              simulation::transformSystemDescription(), *transform_config))
                return lux::cxx::unexpected(EDemoBuildError::SIMULATION_BUILD_FAILURE);
            auto simulation = std::move(simulation_builder).build();
            if (!simulation)
                return lux::cxx::unexpected(EDemoBuildError::SIMULATION_BUILD_FAILURE);

            scene::SceneDescriptionBuilder scene_builder;
            scene_builder.setWorld(assetId(1U));
            scene_builder.setSimulation(assetId(2U));
            scene::RenderSystemConfiguration render_config;
            render_config.coordinate_page_size = coordinate_page_size;
            for (const auto name : {"lux.render.view_camera.v1", "lux.render.material.v1", "lux.render.mesh_stack.v1",
                                    "lux.render.light.v1", "lux.render.forward_mesh.v1", "lux.render.shadow_map.v1",
                                    "lux.render.mesh_shadow.v1"})
            {
                if (!shared_shadows && std::string_view{name} == "lux.render.mesh_shadow.v1")
                    continue;
                const auto *feature = meta.getRenderFeatureMeta(render::featureId(name));
                if (!feature)
                    return lux::cxx::unexpected(EDemoBuildError::META_BUILD_FAILURE);
                scene::RenderFeatureInstanceDescription selected;
                selected.type = feature->type;
                selected.configuration.assign(feature->default_configuration.begin(),
                                              feature->default_configuration.end());
                render_config.features.push_back(std::move(selected));
            }
            const auto render_registration = scene::builtinRenderSystemRegistration();
            std::vector<std::byte> render_bytes;
            if (!render_registration.configuration.encode(&render_config, render_bytes))
                return lux::cxx::unexpected(EDemoBuildError::SCENE_DESCRIPTION_BUILD_FAILURE);
            const auto render_id = system::SystemInstanceId{2};
            const auto &type = scene::RenderSystem::Description;
            if (!scene_builder.addSystem(render_id, "render", render_registration.type, type.version,
                                         type.configuration_schema_name, type.configuration_schema_version,
                                         render_bytes) ||
                !scene_builder.bindRequirement(render_id, "render_runtime", "main-window"))
                return lux::cxx::unexpected(EDemoBuildError::SCENE_DESCRIPTION_BUILD_FAILURE);
            auto description = std::move(scene_builder).build();
            if (!description)
                return lux::cxx::unexpected(EDemoBuildError::SCENE_DESCRIPTION_BUILD_FAILURE);

            const std::array providers{
                scene::makeSceneCapabilityProvider<scene::RenderRuntime>("main-window", "lux.render.runtime", runtime)};
            auto result = scene::Scene::create(
                {std::make_shared<scene::SceneDescription>(std::move(*description)),
                 std::make_shared<world::WorldDescription>(std::move(*world)),
                 std::make_shared<simulation::SimulationDescription>(std::move(*simulation)), meta, providers});
            if (!result)
            {
                std::fprintf(stderr, "Scene build failed: %u / %u / %llu\n", unsigned(result.error().code),
                             unsigned(result.error().scene_system.code),
                             static_cast<unsigned long long>(result.error().scene_system.subject_hash));
                return lux::cxx::unexpected(EDemoBuildError::SCENE_BUILD_FAILURE);
            }
            if (!(*result)->simulation().seal())
                return lux::cxx::unexpected(EDemoBuildError::SIMULATION_BUILD_FAILURE);
            return std::move(*result);
        }
    } // namespace

    lux::cxx::expected<scene::SceneMetaManager, EDemoBuildError> buildDevelopmentSceneMeta() noexcept
    {
        {
            if (!lux::meta::ReflectionRegistry::initialized())
                return lux::cxx::unexpected(EDemoBuildError::META_BUILD_FAILURE);
            // Generated registrars join the host's pending queue. Replaying them
            // directly would leave fix-up pointers into rejected duplicate records.
            lux::meta::ReflectionRegistry::drainPending();
            scene::initializeBuiltinRenderSystemMeta();
            render::initializeBuiltinRenderFeatureMeta();
            std::vector<simulation::ecs::ComponentSchema> schemas;
            const auto append = [&schemas](std::span<const simulation::ecs::ComponentSchema> values)
            { schemas.insert(schemas.end(), values.begin(), values.end()); };
            append(simulation::ecs::transformComponentSchemas());
            append(simulation::ecs::hierarchyComponentSchemas());
            append(simulation::ecs::visualComponentSchemas());
            append(scene::sceneRenderComponentSchemas());
            auto schema_set = simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
            if (!schema_set)
                return lux::cxx::unexpected(EDemoBuildError::SCHEMA_BUILD_FAILURE);

            simulation::SimulationSystemRegistry simulation_systems;
            if (!simulation_systems.add(simulation::transformSystemRegistrations()))
                return lux::cxx::unexpected(EDemoBuildError::META_BUILD_FAILURE);
            const auto render_systems = scene::builtinRenderSystemRegistrations();
            const auto features = render::builtinRenderFeatureRegistrations();
            const auto bindings = scene::builtinRenderFeatureSceneBindings();
            auto meta = scene::SceneMetaManager::build({std::move(*schema_set),
                                                        std::move(simulation_systems),
                                                        {render_systems.begin(), render_systems.end()},
                                                        {features.begin(), features.end()},
                                                        {bindings.begin(), bindings.end()}});
            if (!meta)
            {
                std::fprintf(stderr, "Scene meta failed: %u / %llu\n", unsigned(meta.error().code),
                             static_cast<unsigned long long>(meta.error().subject_hash));
                return lux::cxx::unexpected(EDemoBuildError::META_BUILD_FAILURE);
            }
            return std::move(*meta);
        }
    }

    namespace
    {
        lux::asset::AssetId seed(std::uint8_t tail)
        {
            std::array<std::uint8_t, 16> bytes{};
            bytes[0] = 0x53;
            bytes[1] = 0x56;
            bytes[2] = 1;
            bytes.back() = tail;
            return lux::asset::AssetId{bytes};
        }
        sessions::SceneResult<sessions::SceneOpenInfo> populate(
            sessions::SessionId id, lux::object::ObjectDispatcherRef dispatcher, rendering::EditorRenderer &renderer,
            lux::process::asset_loading::AssetReadPort assets,
            std::shared_ptr<const lux::scene::SceneMetaManager> metadata, bool alternate,
            double coordinate_page_size = 1024.0, bool shared_shadows = false) noexcept
        {
            {
                auto scene = buildDevelopmentScene(*metadata, renderer, coordinate_page_size, shared_shadows);
                if (!scene)
                    return lux::cxx::unexpected(sessions::SceneFailure{sessions::ESceneError::SCENE_BUILD_FAILURE, id});
                sessions::SceneOpenInfo input;
                input.id = id;
                input.dispatcher = std::move(dispatcher);
                input.metadata = std::move(metadata);
                input.scene = std::move(*scene);
                input.asset_read = std::move(assets);
                input.renderer = &renderer;
                auto &registry = input.scene->registry();
                if (alternate)
                {
                    const auto unused = registry.create();
                    registry.destroy(unused);
                    const auto marker = registry.create();
                    registry.emplace<simulation::ecs::Transform3D>(marker);
                    input.labels.emplace(marker, "Survey marker");
                }
                const auto parent = registry.create();
                registry.emplace<simulation::ecs::Transform3D>(parent);
                registry.emplace<simulation::ecs::WorldTransform3D>(parent);
                input.labels.emplace(parent, alternate ? "Inspection set B" : "Development scene");
                const std::array names{"Cube", "Scaled box", "Ground"};
                for (int index = 0; index < 3; ++index)
                {
                    const int object_index = alternate ? 2 - index : index;
                    const auto entity = registry.create();
                    simulation::ecs::Transform3D transform;
                    transform.translation = object_index == 0   ? Eigen::Vector3d{0, 1, 0}
                                            : object_index == 1 ? Eigen::Vector3d{-2, 0.75, -1}
                                                                : Eigen::Vector3d::Zero();
                    if (object_index == 1)
                        transform.scale = {1.5, 1.5, 0.7};
                    if (alternate && object_index == 0)
                        transform.translation.x() = 2;
                    registry.emplace<simulation::ecs::Transform3D>(entity, transform);
                    registry.emplace<simulation::ecs::WorldTransform3D>(entity);
                    simulation::ecs::Mesh3D mesh;
                    mesh.value.mesh = seed(object_index == 2 ? 11 : 10);
                    mesh.value.material = seed(20 + object_index);
                    registry.emplace<simulation::ecs::Mesh3D>(entity, mesh);
                    if (!simulation::ecs::reparent(registry, entity, parent))
                        return lux::cxx::unexpected(
                            sessions::SceneFailure{sessions::ESceneError::SCENE_BUILD_FAILURE, id});
                    input.labels.emplace(entity, names[object_index]);
                    if (!object_index)
                        input.initial_selection = entity;
                }
                const auto light_entity = registry.create();
                simulation::ecs::Transform3D transform;
                transform.translation = {2, 5, 3};
                registry.emplace<simulation::ecs::Transform3D>(light_entity, transform);
                registry.emplace<simulation::ecs::WorldTransform3D>(light_entity);
                simulation::ecs::Light3D light;
                light.value.type = rdesc::ELightType::POINT;
                light.value.intensity = alternate ? 2.0F : 3.0F;
                light.value.range = 30;
                registry.emplace<simulation::ecs::Light3D>(light_entity, light);
                input.labels.emplace(light_entity, "Key light");
                return input;
            }
        }
    } // namespace
    sessions::SceneResult<sessions::SceneOpenInfo> openDevelopmentScene(
        sessions::SessionId id, lux::object::ObjectDispatcherRef dispatcher, rendering::EditorRenderer &renderer,
        lux::process::asset_loading::AssetReadPort assets,
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata) noexcept
    {
        return populate(id, std::move(dispatcher), renderer, std::move(assets), std::move(metadata), false);
    }
    sessions::SceneResult<sessions::SceneEditInput> openDevelopmentEditingScene(
        sessions::SessionId id, lux::object::ObjectDispatcherRef dispatcher, rendering::EditorRenderer &renderer,
        lux::process::asset_loading::AssetReadPort assets,
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata) noexcept
    {
        auto scene = populate(id, std::move(dispatcher), renderer, std::move(assets), std::move(metadata), false);
        if (!scene)
            return lux::cxx::unexpected(scene.error());
        {
            sessions::SceneEditInput input;
            auto &registry = scene->scene->registry();
            std::uint8_t sequence{};
            for (auto entity : registry.view<simulation::ecs::Transform3D>())
            {
                sessions::SceneAuthorObject author;
                author.object = uuidId<world::WorldObjectId>(++sequence);
                author.entity = entity;
                author.transform = registry.get<simulation::ecs::Transform3D>(entity);
                if (const auto *light = registry.try_get<simulation::ecs::Light3D>(entity))
                    author.light = *light;
                input.objects.push_back(std::move(author));
            }
            scene->history_limits = {128, 1024 * 1024, 64 * 1024, 128};
            input.source = std::move(*scene);
            return input;
        }
    }
    sessions::SceneResult<sessions::SceneOpenInfo> openSharedShadowScene(
        sessions::SessionId id, lux::object::ObjectDispatcherRef dispatcher, rendering::EditorRenderer &renderer,
        lux::process::asset_loading::AssetReadPort assets,
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata) noexcept
    {
        return populate(id, std::move(dispatcher), renderer, std::move(assets), std::move(metadata), false, 1024, true);
    }
    sessions::SceneResult<sessions::SceneOpenInfo> openAlternateScene(
        sessions::SessionId id, lux::object::ObjectDispatcherRef dispatcher, rendering::EditorRenderer &renderer,
        lux::process::asset_loading::AssetReadPort assets,
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata) noexcept
    {
        return populate(id, std::move(dispatcher), renderer, std::move(assets), std::move(metadata), true);
    }
    sessions::SceneResult<sessions::SceneOpenInfo> openCoordinateScene(
        sessions::SessionId id, lux::object::ObjectDispatcherRef dispatcher, rendering::EditorRenderer &renderer,
        lux::process::asset_loading::AssetReadPort assets,
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata, double page_size) noexcept
    {
        return populate(id, std::move(dispatcher), renderer, std::move(assets), std::move(metadata), false, page_size);
    }
} // namespace lux::editor::examples
