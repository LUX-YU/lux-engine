#include "EditorBootstrap.hpp"

#include <lux/engine/editor/inspector/EntityInspector.hpp>
#include <lux/engine/editor/inspector/FirstPartyComponentEditors.hpp>
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

namespace lux::editor::application
{
    namespace
    {
        template<class Type>
        [[nodiscard]] Type uuidId(std::uint8_t tail)
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

        [[nodiscard]] lux::cxx::expected<std::unique_ptr<scene::Scene>, EEditorBootstrapError>
        buildDevelopmentScene(const scene::SceneMetaManager& meta) noexcept
        {
            world::WorldDescriptionBuilder world_builder;
            if (!world_builder.setIdentity(
                    uuidId<world::WorldBundleId>(1U),
                    uuidId<world::WorldBundleGeneration>(1U),
                    "lux-editor-development-scene"
                ) ||
                !world_builder.setPartitioner({world::worldPartitionerId("lux.editor.none"), 1U}, 0U))
            {
                return lux::cxx::unexpected(EEditorBootstrapError::WORLD_BUILD_FAILURE);
            }
            auto world = std::move(world_builder).build();
            if (!world)
                return lux::cxx::unexpected(EEditorBootstrapError::WORLD_BUILD_FAILURE);

            simulation::SimulationDescriptionBuilder simulation_builder;
            auto simulation = std::move(simulation_builder).build();
            if (!simulation)
                return lux::cxx::unexpected(EEditorBootstrapError::SIMULATION_BUILD_FAILURE);

            scene::SceneDescriptionBuilder scene_builder;
            scene_builder.setWorld(assetId(1U));
            scene_builder.setSimulation(assetId(2U));
            auto description = std::move(scene_builder).build();
            if (!description)
                return lux::cxx::unexpected(EEditorBootstrapError::SCENE_DESCRIPTION_BUILD_FAILURE);

            auto result = scene::Scene::create({
                std::make_shared<scene::SceneDescription>(std::move(*description)),
                std::make_shared<world::WorldDescription>(std::move(*world)),
                std::make_shared<simulation::SimulationDescription>(std::move(*simulation)),
                meta,
                {}
            });
            if (!result)
                return lux::cxx::unexpected(EEditorBootstrapError::SCENE_BUILD_FAILURE);
            return std::move(*result);
        }
    } // namespace

    struct EditorBootstrap::Impl final
    {
        ~Impl() noexcept
        {
            pane_registration.reset();
            inspector.reset();
            if (context && scene_handle.valid())
                static_cast<void>(context->selection().deactivate(scene_handle));
            scene.reset();
        }

        EditorContext* context{};
        std::unique_ptr<scene::Scene> scene;
        EditorSceneHandle scene_handle{};
        std::unique_ptr<inspector::EntityInspector> inspector;
        ui::PaneRegistration pane_registration;
    };

    lux::cxx::expected<scene::SceneMetaManager, EEditorBootstrapError> buildDevelopmentSceneMeta() noexcept
    {
        try
        {
            std::vector<simulation::ecs::ComponentSchema> schemas;
            const auto append = [&schemas](std::span<const simulation::ecs::ComponentSchema> values) {
                schemas.insert(schemas.end(), values.begin(), values.end());
            };
            append(simulation::ecs::transformComponentSchemas());
            append(simulation::ecs::hierarchyComponentSchemas());
            append(simulation::ecs::visualComponentSchemas());
            auto schema_set = simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
            if (!schema_set)
                return lux::cxx::unexpected(EEditorBootstrapError::SCHEMA_BUILD_FAILURE);

            auto meta = scene::SceneMetaManager::build({
                std::move(*schema_set),
                simulation::SimulationSystemRegistry{},
                {},
                {},
                {}
            });
            if (!meta)
                return lux::cxx::unexpected(EEditorBootstrapError::META_BUILD_FAILURE);
            return std::move(*meta);
        }
        catch (...)
        {
            return lux::cxx::unexpected(EEditorBootstrapError::ALLOCATION_FAILURE);
        }
    }

    EditorBootstrap::EditorBootstrap(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    EditorBootstrap::CreateResult EditorBootstrap::create(EditorContext& context) noexcept
    {
        try
        {
            auto scene_result = buildDevelopmentScene(context.sceneMeta());
            if (!scene_result)
                return lux::cxx::unexpected(scene_result.error());

            auto bindings = inspector::buildFirstPartyComponentEditorBindings();
            if (!bindings)
                return lux::cxx::unexpected(EEditorBootstrapError::BINDING_BUILD_FAILURE);

            auto impl = std::make_unique<Impl>();
            impl->context = &context;
            impl->scene = std::move(*scene_result);

            auto& registry = impl->scene->registry();
            const auto parent = registry.create();
            registry.emplace<simulation::ecs::Transform3D>(parent);
            registry.emplace<simulation::ecs::WorldTransform3D>(parent);

            const auto selected = registry.create();
            auto& transform = registry.emplace<simulation::ecs::Transform3D>(selected);
            transform.translation = Eigen::Vector3d{0.0, 1.0, 0.0};
            registry.emplace<simulation::ecs::WorldTransform3D>(selected);
            registry.emplace<simulation::ecs::Mesh3D>(selected);
            registry.emplace<simulation::ecs::Light3D>(selected);
            if (!simulation::ecs::reparent(registry, selected, parent))
                return lux::cxx::unexpected(EEditorBootstrapError::SELECTION_FAILURE);

            impl->inspector = std::make_unique<inspector::EntityInspector>(
                context.ui().dispatcherRef(),
                ui::PaneId{"lux-editor.entity-inspector"},
                context,
                std::move(*bindings)
            );
            auto registration = context.ui().registerPane(*impl->inspector);
            if (!registration)
                return lux::cxx::unexpected(EEditorBootstrapError::PANE_REGISTRATION_FAILURE);
            impl->pane_registration = std::move(*registration);

            impl->scene_handle = context.selection().activate(*impl->scene);
            if (!impl->scene_handle.valid() || !context.selection().select(impl->scene_handle, selected))
                return lux::cxx::unexpected(EEditorBootstrapError::SELECTION_FAILURE);
            return std::unique_ptr<EditorBootstrap>{new EditorBootstrap(std::move(impl))};
        }
        catch (...)
        {
            return lux::cxx::unexpected(EEditorBootstrapError::ALLOCATION_FAILURE);
        }
    }

    EditorBootstrap::~EditorBootstrap() noexcept = default;
} // namespace lux::editor::application
