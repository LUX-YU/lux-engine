#include <algorithm>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/project/PluginRendering.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>

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
            const Configuration& configuration
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
            const bool wrong_codec = found->configuration.type != cxx::typeToken<Configuration>();
            const bool supported_world = system::supportsWorldType(description, "lux.spatial.builtin.single");
            if (wrong_version || wrong_codec || !supported_world)
            {
                return cxx::unexpected(missing("3d profile incompatible system", expected.canonical_name));
            }
            std::vector<std::byte> bytes;
            if (auto encoded = found->configuration.encode(&configuration, bytes); !encoded)
            {
                return cxx::unexpected(scene::ScenePackageFailure{
                    scene::EScenePackageError::ENCODE,
                    {},
                    0,
                    encoded.error(),
                    std::string{expected.canonical_name}
                });
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

            // The preset is product policy. FeatureCatalog owns dependency resolution. The project owns availability
            // and codecs; this path never registers features with a live RenderRuntime.
            constexpr std::string_view feature_names[]{
                "lux.render.material.v1",
                "lux.render.mesh_stack.v1",
                "lux.render.view_camera.v1",
                "lux.render.light.v1",
                "lux.render.forward_mesh.v1"
            };
            scene::RenderSystemConfiguration rendering;
            for (std::size_t i{}; i < std::size(feature_names); ++i)
            {
                const auto found = std::ranges::find_if(
                    info.registrations.features,
                    [&](const auto& entry) { return entry.factory.descriptor.canonical_name == feature_names[i]; }
                );
                if (found == info.registrations.features.end())
                {
                    return cxx::unexpected(missing("3d profile feature", feature_names[i]));
                }
                const bool invalid = !found->scene_configurable || !found->configuration.valid() ||
                                     found->factory.descriptor.type != render::featureId(feature_names[i]);
                if (invalid)
                {
                    return cxx::unexpected(missing("3d profile feature configuration", feature_names[i]));
                }
                scene::RenderFeatureInstanceDescription instance;
                instance.type = found->factory.descriptor.type;
                instance.configuration_schema = found->configuration.schema;
                instance.configuration_version = found->configuration.schema_version;
                auto encoded = found->configuration.portable.encode_default(instance.configuration);
                if (!encoded)
                {
                    return cxx::unexpected(scene::ScenePackageFailure{
                        scene::EScenePackageError::ENCODE,
                        {},
                        i,
                        encoded.error(),
                        "3d profile feature configuration"
                    });
                }
                rendering.features.push_back(std::move(instance));
            }
            for (const auto feature : {"lux.render.view_camera.v1", "lux.render.mesh_stack.v1", "lux.render.light.v1"})
            {
                const bool has_binding = std::ranges::any_of(
                    info.registrations.render_bindings,
                    [&](const auto& binding)
                    { return binding.feature == render::featureId(feature) && binding.create_sync_stage != nullptr; }
                );
                if (!has_binding)
                {
                    return cxx::unexpected(missing("3d profile render binding", feature));
                }
            }

            scene::SceneDescriptionBuilder builder;
            if (auto result = addSystem(
                    builder,
                    info.registrations,
                    scene::TransformSystem::Description,
                    {1},
                    "transform",
                    scene::TransformSystemConfiguration{4096, 32768, 1U << 20U}
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
                    scene::WorldLoadingConfiguration{{partition::PartitionOrdinal{0}}}
                );
                !result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
            if (auto result =
                    addSystem(builder, info.registrations, scene::RenderSystem::Description, {3}, "render", rendering);
                !result)
            {
                return cxx::unexpected(std::move(result.error()));
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
