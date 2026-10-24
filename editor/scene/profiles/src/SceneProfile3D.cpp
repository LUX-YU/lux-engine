#include <algorithm>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/project/SceneRegistrations.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneCompatibility.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <map>

namespace lux::editor
{
    namespace
    {
        scene::ScenePackageFailure missing(std::string_view stage, std::string_view name)
        {
            return {scene::EScenePackageError::INVALID_ARGUMENT, {}, 0, std::string{name}, std::string{stage}};
        }

        template <class Configuration>
        cxx::expected<void, scene::ScenePackageFailure> addSystem(
            scene::SceneDescriptionBuilder& builder,
            const project::SceneRegistrations& registrations,
            const system::SystemTypeDescription& expected,
            system::SystemInstanceId id,
            std::string_view name,
            const Configuration* configuration
        ) noexcept
        {
            const auto found = std::ranges::find_if(
                registrations.scene_systems,
                [&](const auto& entry) { return entry.type.name == expected.canonical_name; }
            );
            if (found == registrations.scene_systems.end())
            {
                return cxx::unexpected(missing("3d profile system", expected.canonical_name));
            }
            if (!scene::validSceneSystemRegistration(*found))
            {
                return cxx::unexpected(missing("3d profile invalid system", expected.canonical_name));
            }
            const auto& description = *found->description;
            const bool wrong_version =
                description.version != expected.version ||
                description.configuration_schema_name != expected.configuration_schema_name ||
                description.configuration_schema_version != expected.configuration_schema_version;
            const bool wrong_codec = [&]() noexcept
            {
                if constexpr (std::is_void_v<Configuration>)
                {
                    return found->configuration.valid();
                }
                else
                {
                    return found->configuration.type != cxx::typeToken<Configuration>();
                }
            }();
            if (wrong_version || wrong_codec)
            {
                return cxx::unexpected(missing("3d profile incompatible system", expected.canonical_name));
            }
            std::vector<std::byte> bytes;
            if constexpr (!std::is_void_v<Configuration>)
            {
                if (auto encoded = found->configuration.encode(configuration, bytes); !encoded)
                {
                    return cxx::unexpected(scene::ScenePackageFailure{
                        scene::EScenePackageError::ENCODE,
                        {},
                        0,
                        encoded.error(),
                        std::string{expected.canonical_name}
                    });
                }
            }
            auto added = builder.addSystem(
                id,
                name,
                found->type,
                description.version,
                description.configuration_schema_name,
                description.configuration_schema_version,
                bytes
            );
            if (!added)
            {
                return cxx::unexpected(scene::ScenePackageFailure{
                    scene::EScenePackageError::INVALID_ARGUMENT,
                    {},
                    0,
                    added.error(),
                    "3d profile system instance"
                });
            }
            return {};
        }

        SceneCreateResult create3D(const SceneCreateInfo& info) noexcept
        {
            if (info.stop.stop_requested())
            {
                return cxx::unexpected(scene::ScenePackageFailure{scene::EScenePackageError::CANCELLED});
            }
            if (info.selection)
            {
                const auto& selected = *info.selection;
                if (!selected.simulation || !selected.systems)
                {
                    return cxx::unexpected(missing("Scene selection", "missing system description"));
                }
                if (selected.partitioner.id.name != "lux.spatial.builtin.single" || selected.partitioner.version != 1)
                {
                    return cxx::unexpected(scene::ScenePackageFailure{
                        scene::EScenePackageError::INDEX_REBUILD_REQUIRED,
                        info.id,
                        0,
                        {},
                        "Creating an indexed World requires its actual index builder"
                    });
                }
                std::vector<scene::SceneSystemContract> systems;
                std::vector<system::SystemDataContract> schemas;
                std::map<system::SystemInstanceId, std::size_t> scene_indices;
                for (const auto& name : selected.schemas)
                {
                    const auto* schema =
                        info.registrations.components.find(simulation::ecs::componentSchemaId(name.name));
                    if (!schema)
                    {
                        return cxx::unexpected(missing("Scene schema", name.name));
                    }
                    const bool is_not_authored =
                        schema->semantic_kind == simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                        schema->snapshot != simulation::ecs::EComponentSnapshotPolicy::COPY;
                    if (is_not_authored)
                    {
                        return cxx::unexpected(missing("Scene schema is not authored", name.name));
                    }
                    schemas.push_back({name.name, schema->version});
                }
                for (std::size_t i = 0; i < selected.simulation->systemCount(); ++i)
                {
                    const auto input = selected.simulation->systemAt(i);
                    const auto* registration = info.registrations.simulation_systems
                                                   ? info.registrations.simulation_systems->find(input.type())
                                                   : nullptr;
                    if (!registration)
                    {
                        return cxx::unexpected(missing("Simulation system", input.type().name));
                    }
                    systems.push_back({input.instanceId(), &registration->description->type, true});
                }
                for (std::size_t i = 0; i < selected.systems->systemCount(); ++i)
                {
                    const auto input = selected.systems->systemAt(i);
                    const auto found = std::ranges::find(
                        info.registrations.scene_systems,
                        input.type(),
                        &scene::SceneSystemRegistration::type
                    );
                    if (found == info.registrations.scene_systems.end())
                    {
                        return cxx::unexpected(missing("Scene system", input.type().name));
                    }
                    scene_indices.emplace(input.instanceId(), systems.size());
                    systems.push_back({input.instanceId(), found->description, false});
                }
                scene::SceneDataDependencies dependencies;
                for (std::size_t i = 0; i < selected.systems->dependencyCount(); ++i)
                {
                    const auto dependency = selected.systems->dependencyAt(i);
                    dependencies.push_back(
                        {scene_indices.find(dependency.before())->second, scene_indices.find(dependency.after())->second
                        }
                    );
                }
                auto compatible = scene::resolveSceneData(systems, schemas, dependencies);
                if (!compatible)
                {
                    return cxx::unexpected(scene::ScenePackageFailure{
                        scene::EScenePackageError::INVALID_ARGUMENT,
                        info.id,
                        static_cast<std::size_t>(compatible.error().code),
                        compatible.error().data,
                        "Scene data compatibility"
                    });
                }
                return scene::createScenePackage(
                    info.id,
                    info.name,
                    selected.schemas,
                    selected.simulation,
                    *selected.systems
                );
            }
            // These are authored schema requirements of this profile, not runtime-derived pools.
            constexpr std::string_view schema_names[]{
                "lux.ecs.Parent",
                "lux.ecs.Transform3D",
                "lux.ecs.Mesh3D",
                "lux.ecs.Light3D",
                "lux.scene.Camera"
            };
            std::vector<world::WorldDataSchemaId> schemas;
            for (const auto name : schema_names)
            {
                const auto* schema = info.registrations.components.find(simulation::ecs::componentSchemaId(name));
                if (!schema)
                {
                    return cxx::unexpected(missing("3d profile schema", name));
                }
                const bool not_authored =
                    schema->semantic_kind == simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                    schema->snapshot != simulation::ecs::EComponentSnapshotPolicy::COPY;
                if (not_authored)
                {
                    return cxx::unexpected(missing("3d profile non-author schema", name));
                }
                schemas.push_back(world::worldDataSchemaId(schema->id.name));
            }

            // The profile chooses Scene systems. Application startup registers Render factories;
            // RenderSystem composes an Effect from actual cooked material/geometry groups.
            const scene::TransformSystemConfiguration transform{4096, 32768, 1U << 20U};
            const scene::WorldLoadingConfiguration world_loading{{partition::PartitionOrdinal{0}}};
            scene::SceneDescriptionBuilder builder;
            if (auto result = addSystem(
                    builder,
                    info.registrations,
                    scene::TransformSystem::Description,
                    {1},
                    "transform",
                    &transform
                );
                !result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
            if (auto result = addSystem(
                    builder,
                    info.registrations,
                    scene::WorldLoadingSystem::Description,
                    {2},
                    "world_loading",
                    &world_loading
                );
                !result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
            const scene::RenderSystemConfiguration rendering;
            if (auto result =
                    addSystem(builder, info.registrations, scene::RenderSystem::Description, {3}, "render", &rendering);
                !result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
            if (auto ordered = builder.addDependency({1}, {3}); !ordered)
            {
                return cxx::unexpected(scene::ScenePackageFailure{
                    scene::EScenePackageError::INVALID_ARGUMENT,
                    {},
                    0,
                    ordered.error(),
                    "3d transform/render order"
                });
            }
            if (auto query = addSystem<
                    void>(builder, info.registrations, scene::MeshQuerySystem::Description, {4}, "mesh_query", nullptr);
                !query)
            {
                return cxx::unexpected(query.error());
            }
            auto description = std::move(builder).buildResolved();
            if (!description)
            {
                return cxx::unexpected(scene::ScenePackageFailure{
                    scene::EScenePackageError::INVALID_ARGUMENT,
                    info.id,
                    0,
                    description.error(),
                    "3d profile description"
                });
            }
            if (info.stop.stop_requested())
            {
                return cxx::unexpected(scene::ScenePackageFailure{scene::EScenePackageError::CANCELLED});
            }
            return scene::createScenePackage(
                info.id,
                info.name,
                schemas,
                std::make_shared<const simulation::SimulationDescription>(),
                *description
            );
        }
    } // namespace

    SceneProfileRegistration sceneProfile3D() noexcept
    {
        return {
            {},
            "lux.editor.scene.3d",
            "3D Scene",
            {"spatial.3d", "transform.3d", "mesh.3d", "camera.3d"},
            &create3D
        };
    }
} // namespace lux::editor
