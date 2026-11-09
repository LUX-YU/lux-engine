#include <algorithm>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/plugins/SceneRegistrations.hpp>
#include <lux/engine/render/capabilities/RenderPresets.hpp>
#include <lux/engine/scene/FixedPartitionStreamingSystem.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/systems/render/RenderSystemConfiguration.hpp>

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
            const plugins::SceneRegistrations& registrations,
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
                    return found->configuration.type != cxx::make_basic_type_info<Configuration>();
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
                return createSelectedScene(info);
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
                const auto* schema = info.registrations.components.find(ecs::componentSchemaId(name));
                if (!schema)
                {
                    return cxx::unexpected(missing("3d profile schema", name));
                }
                const bool not_authored =
                    schema->semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                    schema->snapshot != ecs::EComponentSnapshotPolicy::COPY;
                if (not_authored)
                {
                    return cxx::unexpected(missing("3d profile non-author schema", name));
                }
                schemas.push_back(world::worldDataSchemaId(schema->id.name));
            }

            // The profile chooses default capability demand and implementation. Render owns its effect composition.
            const scene::TransformSystemConfiguration transform{4096, 32768, 1U << 20U};
            const scene::WorldLoadingConfiguration world_loading{};
            scene::SceneDescriptionBuilder builder;
            const auto geometry = std::ranges::find(
                info.registrations.geometries,
                std::string_view{"lux.geometry.mesh3d"},
                [](const auto& value) { return std::string_view(value.name); }
            );
            if (geometry == info.registrations.geometries.end() || geometry->version != 1)
            {
                return cxx::unexpected(missing("3d profile geometry", "lux.geometry.mesh3d"));
            }
            if (auto chosen = builder.selectGeometry(
                    {{geometry->name, geometry->version}, std::string(world::kDefaultWorldDomain)}
                );
                !chosen)
            {
                return cxx::unexpected(missing("3d profile geometry", geometry->name));
            }

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
            const scene::WorldStreamingDemand demand{{partition::PartitionOrdinal{0}}, true};
            if (auto added = addSystem(
                    builder,
                    info.registrations,
                    scene::FixedPartitionStreamingSystem::Description,
                    {5},
                    "streaming",
                    &demand
                );
                !added)
            {
                return cxx::unexpected(added.error());
            }
            if (auto ordered = builder.addDependency({5}, {2}); !ordered)
            {
                return cxx::unexpected(missing("streaming dependency", "world_loading"));
            }
            ::lux::engine::render::RenderSystemConfiguration rendering;
            rendering.composition = lux::render::deferredRenderComposition();
            if (auto result = addSystem(
                    builder,
                    info.registrations,
                    ::lux::engine::render::RenderSystem::Description,
                    {3},
                    "render",
                    &rendering
                );
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
                *description,
                SceneCreateSelection{}.domains,
                info.registrations.partitions,
                info.stop
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
