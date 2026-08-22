#include <lux/engine/toolchain/game_export/GameBuildClosure.hpp>

#undef TOML_HEADER_ONLY
#define TOML_HEADER_ONLY 1
#undef TOML_EXCEPTIONS
#define TOML_EXCEPTIONS 0
#undef TOML_SHARED_LIB
#define TOML_SHARED_LIB 0
#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace lux::toolchain::detail
{
    namespace
    {
        constexpr std::uint32_t kProjectUsageSchema = 1u;
        constexpr std::string_view kUsageManifestName =
            "ProjectUsageManifest.toml";
        constexpr std::string_view kCompositionSourceName =
            "GameComposition.cpp";

        enum class ESystemRecipe : std::uint8_t
        {
            TRANSFORM_3D,
            ANIMATION_3D,
            PHYSICS_3D,
            NAVIGATION_3D,
            TRANSFORM_2D,
            SIMULATION_2D,
            TILEMAP_2D,
            SPATIAL_3D,
            PRESENTATION_3D,
            PRESENTATION_2D
        };

        using ComponentMap = std::map<
            std::string,
            lux::ecs::scene_format::RequiredComponentSchema,
            std::less<>>;
        using ExtensionMap = std::map<
            std::string,
            lux::scene::RequiredExtension,
            std::less<>>;

        [[nodiscard]] GameExportFailure failure(
            EGameExportError code,
            std::string detail)
        {
            return {code, std::move(detail)};
        }

        [[nodiscard]] bool contains(
            std::string_view text,
            std::string_view token) noexcept
        {
            return text.find(token) != std::string_view::npos;
        }

        [[nodiscard]] std::string hashText(std::uint64_t value)
        {
            std::ostringstream stream;
            stream << "0x" << std::hex << std::setfill('0') <<
                std::setw(16) << value;
            return std::move(stream).str();
        }

        [[nodiscard]] std::string_view recipeName(
            ESystemRecipe recipe) noexcept
        {
            switch (recipe)
            {
            case ESystemRecipe::TRANSFORM_3D:
                return "lux.ecs.installSpatial3DTransformSystems";
            case ESystemRecipe::ANIMATION_3D:
                return "lux.ecs.installAnimation3DSystems";
            case ESystemRecipe::PHYSICS_3D:
                return "lux.runtime.installPhysics3DSystems";
            case ESystemRecipe::NAVIGATION_3D:
                return "lux.runtime.installNavigation3DSystems";
            case ESystemRecipe::TRANSFORM_2D:
                return "lux.ecs.installSpatial2DTransformSystems";
            case ESystemRecipe::SIMULATION_2D:
                return "lux.ecs.installSimulation2DSystems";
            case ESystemRecipe::TILEMAP_2D:
                return "lux.runtime.installTilemap2DSystems";
            case ESystemRecipe::SPATIAL_3D:
                return "lux.runtime.installSpatial3DSystems";
            case ESystemRecipe::PRESENTATION_3D:
                return "lux.runtime.installPresentation3DSystems";
            case ESystemRecipe::PRESENTATION_2D:
                return "lux.ecs.installPresentation2DSystems";
            }
            return {};
        }

        [[nodiscard]] std::set<ESystemRecipe> deriveSystemRecipes(
            const ProjectBuildUsage& usage)
        {
            std::set<ESystemRecipe> result;
            bool has_2d_presentation = false;
            bool has_3d_presentation = false;
            for (const auto& component : usage.required_components)
            {
                const auto name = std::string_view{component.id.name};
                if (contains(name, "transform3dcomponent"))
                    result.emplace(ESystemRecipe::TRANSFORM_3D);
                if (contains(name, "animatorcomponent") ||
                    contains(name, "skeletalmeshcomponent"))
                {
                    result.emplace(ESystemRecipe::ANIMATION_3D);
                }
                if (contains(name, "rigidbody3dcomponent") ||
                    contains(name, "collider3dcomponent") ||
                    contains(name, "staticcolliderbatch3dcomponent") ||
                    contains(name, "charactercontroller3dcomponent"))
                {
                    result.emplace(ESystemRecipe::PHYSICS_3D);
                }
                if (contains(name, "navigationregion3dcomponent"))
                    result.emplace(ESystemRecipe::NAVIGATION_3D);
                if (contains(name, "transform2dcomponent"))
                    result.emplace(ESystemRecipe::TRANSFORM_2D);
                if (contains(name, "tilemapcomponent") ||
                    contains(name, "tilechunk2dcomponent"))
                {
                    result.emplace(ESystemRecipe::TILEMAP_2D);
                }
            }
            for (const auto& feature : usage.required_render_features)
            {
                if (feature == "Canvas2D" || feature == "Grid2D")
                    has_2d_presentation = true;
                else
                    has_3d_presentation = true;
            }
            if (has_2d_presentation)
            {
                result.emplace(ESystemRecipe::SIMULATION_2D);
                result.emplace(ESystemRecipe::PRESENTATION_2D);
            }
            if (usage.spatial3d_streaming)
                result.emplace(ESystemRecipe::SPATIAL_3D);
            if (has_3d_presentation)
                result.emplace(ESystemRecipe::PRESENTATION_3D);
            return result;
        }

        void addIncludes(
            std::ostringstream& source,
            const std::set<ESystemRecipe>& recipes,
            bool renderer)
        {
            const auto has = [&recipes](ESystemRecipe recipe)
            {
                return recipes.contains(recipe);
            };
            source <<
                "// Generated by lux_game_exporter. Build input only; do not install.\n"
                "#include <lux/engine/ecs/ComponentTypeCatalog.hpp>\n"
                "#include <lux/engine/ecs/ScheduleBuilder.hpp>\n";
            if (has(ESystemRecipe::TRANSFORM_3D) ||
                has(ESystemRecipe::TRANSFORM_2D))
            {
                source << "#include <lux/engine/ecs/transform/InstallTransformSystems.hpp>\n";
            }
            if (has(ESystemRecipe::ANIMATION_3D))
                source << "#include <lux/engine/ecs/animation/InstallAnimationSystems.hpp>\n";
            if (has(ESystemRecipe::SIMULATION_2D))
                source << "#include <lux/engine/ecs/physics/InstallSimulationSystems.hpp>\n";
            if (has(ESystemRecipe::PRESENTATION_2D))
                source << "#include <lux/engine/ecs/integration/presentation2d/InstallPresentation2DSystems.hpp>\n";
            if (has(ESystemRecipe::PHYSICS_3D))
                source << "#include <lux/engine/runtime/scene/composition/InstallPhysics3DSystems.hpp>\n";
            if (has(ESystemRecipe::NAVIGATION_3D))
                source << "#include <lux/engine/runtime/scene/composition/InstallNavigation3DSystems.hpp>\n";
            if (has(ESystemRecipe::TILEMAP_2D))
                source << "#include <lux/engine/runtime/scene/composition/InstallTilemapSystems.hpp>\n";
            if (has(ESystemRecipe::SPATIAL_3D))
                source << "#include <lux/engine/runtime/scene/composition/InstallSpatial3DSystems.hpp>\n";
            if (has(ESystemRecipe::PRESENTATION_3D))
                source << "#include <lux/engine/runtime/scene/composition/InstallPresentation3DSystems.hpp>\n";
            if (renderer)
            {
                source <<
                    "#include <lux/engine/runtime/render/scene/StandardFeaturePlan.hpp>\n"
                    "#include <lux/engine/function/render/client/FeatureCatalog.hpp>\n"
                    "#include <vector>\n";
            }
            source << '\n';
        }

        void addSystemParameters(
            std::ostringstream& source,
            const std::set<ESystemRecipe>& recipes)
        {
            if (recipes.contains(ESystemRecipe::PHYSICS_3D))
            {
                source <<
                    ",\n        lux::ecs::physics3d::streaming::StaticCollider3DPrepareClient physics3d";
            }
            if (recipes.contains(ESystemRecipe::NAVIGATION_3D))
            {
                source <<
                    ",\n        lux::ecs::navigation::streaming::Navigation3DPrepareClient navigation3d";
            }
            if (recipes.contains(ESystemRecipe::TILEMAP_2D))
            {
                source <<
                    ",\n        lux::ecs::tilemap::streaming::TilemapPrepareClient tilemap2d";
            }
            if (recipes.contains(ESystemRecipe::PRESENTATION_3D))
            {
                source <<
                    ",\n        lux::runtime::ClassicMeshPrepareClient classic_mesh"
                    ",\n        lux::runtime::TerrainPrepareClient terrain";
            }
        }

        void addSystemCall(
            std::ostringstream& source,
            ESystemRecipe recipe)
        {
            source << "            ";
            switch (recipe)
            {
            case ESystemRecipe::TRANSFORM_3D:
                source << "lux::ecs::installSpatial3DTransformSystems(builder, components)";
                break;
            case ESystemRecipe::ANIMATION_3D:
                source << "lux::ecs::installAnimation3DSystems(builder, components)";
                break;
            case ESystemRecipe::PHYSICS_3D:
                source << "lux::runtime::installPhysics3DSystems(builder, components, physics3d)";
                break;
            case ESystemRecipe::NAVIGATION_3D:
                source << "lux::runtime::installNavigation3DSystems(builder, components, navigation3d)";
                break;
            case ESystemRecipe::TRANSFORM_2D:
                source << "lux::ecs::installSpatial2DTransformSystems(builder, components)";
                break;
            case ESystemRecipe::SIMULATION_2D:
                source << "lux::ecs::installSimulation2DSystems(builder, components)";
                break;
            case ESystemRecipe::TILEMAP_2D:
                source << "lux::runtime::installTilemap2DSystems(builder, components, tilemap2d)";
                break;
            case ESystemRecipe::SPATIAL_3D:
                source << "lux::runtime::installSpatial3DSystems(builder, components)";
                break;
            case ESystemRecipe::PRESENTATION_3D:
                source << "lux::runtime::installPresentation3DSystems(builder, components, classic_mesh, terrain)";
                break;
            case ESystemRecipe::PRESENTATION_2D:
                source << "lux::ecs::installPresentation2DSystems(builder, components)";
                break;
            }
        }

        [[nodiscard]] lux::cxx::expected<void, GameExportFailure>
        writeText(
            const std::filesystem::path& path,
            std::string_view text)
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (!stream.is_open())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::MANIFEST_WRITE_FAILED,
                    "cannot create build artifact '" + path.string() + "'"));
            }
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.flush();
            if (!stream.good())
            {
                return lux::cxx::unexpected(failure(
                    EGameExportError::MANIFEST_WRITE_FAILED,
                    "failed while writing build artifact '" +
                        path.string() + "'"));
            }
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, GameExportFailure>
        canonicalize(ProjectBuildUsage& usage)
        {
            ComponentMap components;
            for (auto& component : usage.required_components)
            {
                const auto name = component.id.name;
                const auto [found, inserted] = components.emplace(
                    name, component);
                if (!inserted &&
                    (found->second.id.hash != component.id.hash ||
                     found->second.schema_version != component.schema_version))
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::COOK_FAILED,
                        "component schema has conflicting build requirements: '" +
                            name + "'"));
                }
            }
            usage.required_components.clear();
            for (auto& [_, component] : components)
                usage.required_components.push_back(std::move(component));

            const auto canonicalize_features = [](std::vector<std::string>& values)
            {
                std::ranges::sort(values);
                values.erase(
                    std::unique(values.begin(), values.end()),
                    values.end());
            };
            canonicalize_features(usage.required_render_features);
            canonicalize_features(usage.optional_render_features);
            usage.optional_render_features.erase(
                std::remove_if(
                    usage.optional_render_features.begin(),
                    usage.optional_render_features.end(),
                    [&usage](const std::string& value)
                    {
                        return std::binary_search(
                            usage.required_render_features.begin(),
                            usage.required_render_features.end(),
                            value);
                    }),
                usage.optional_render_features.end());

            ExtensionMap scene_extensions;
            for (auto& extension : usage.scene_required_extensions)
            {
                const auto name = std::string{extension.id.name()};
                const auto [found, inserted] = scene_extensions.emplace(
                    name, extension);
                if (!inserted)
                {
                    if (found->second.required_major !=
                        extension.required_major)
                    {
                        return lux::cxx::unexpected(failure(
                            EGameExportError::COOK_FAILED,
                            "Scenes require incompatible major versions of extension '" +
                                name + "'"));
                    }
                    found->second.minimum_minor = std::max(
                        found->second.minimum_minor,
                        extension.minimum_minor);
                }
            }
            usage.scene_required_extensions.clear();
            for (auto& [_, extension] : scene_extensions)
                usage.scene_required_extensions.push_back(std::move(extension));

            std::ranges::sort(
                usage.selected_extensions,
                {},
                [](const BuildExtension& value)
                {
                    return value.id.name();
                });
            for (std::size_t index = 1u;
                 index < usage.selected_extensions.size(); ++index)
            {
                if (usage.selected_extensions[index - 1u].id.hash() ==
                    usage.selected_extensions[index].id.hash())
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::PROJECT_INVALID,
                        "project selects duplicate or colliding extension '" +
                            std::string{usage.selected_extensions[index].id.name()} +
                            "'"));
                }
            }
            for (const auto& required : usage.scene_required_extensions)
            {
                const auto found = std::ranges::find_if(
                    usage.selected_extensions,
                    [&required](const BuildExtension& selected)
                    {
                        return selected.id.hash() == required.id.hash();
                    });
                if (found == usage.selected_extensions.end() ||
                    found->required_major != required.required_major ||
                    found->minimum_minor < required.minimum_minor)
                {
                    return lux::cxx::unexpected(failure(
                        EGameExportError::EXTENSION_DEPENDENCY_MISSING,
                        "project selection does not satisfy Scene extension '" +
                            std::string{required.id.name()} + "'"));
                }
            }
            return {};
        }

        [[nodiscard]] std::string makeUsageManifest(
            const ProjectBuildUsage& usage,
            const std::set<ESystemRecipe>& recipes)
        {
            toml::table project_usage{
                {"schema", static_cast<std::int64_t>(kProjectUsageSchema)},
                {"binary_name", usage.binary_name},
                {"renderer", !usage.required_render_features.empty() ||
                    !usage.optional_render_features.empty()},
                {"spatial3d_streaming", usage.spatial3d_streaming}};
            toml::array systems;
            for (const auto recipe : recipes)
                systems.push_back(std::string{recipeName(recipe)});
            project_usage.insert("systems", std::move(systems));
            toml::table root{{"project_usage", std::move(project_usage)}};

            toml::array components;
            for (const auto& component : usage.required_components)
            {
                components.push_back(toml::table{
                    {"name", component.id.name},
                    {"hash", hashText(component.id.hash)},
                    {"version", static_cast<std::int64_t>(
                                    component.schema_version)}});
            }
            root.insert("components", std::move(components));

            toml::array required_features;
            for (const auto& feature : usage.required_render_features)
                required_features.push_back(feature);
            root.insert("required_render_features", std::move(required_features));
            toml::array optional_features;
            for (const auto& feature : usage.optional_render_features)
                optional_features.push_back(feature);
            root.insert("optional_render_features", std::move(optional_features));

            toml::array extensions;
            for (const auto& extension : usage.selected_extensions)
            {
                extensions.push_back(toml::table{
                    {"id", std::string{extension.id.name()}},
                    {"source", extension.source.generic_string()},
                    {"major", static_cast<std::int64_t>(
                                  extension.required_major)},
                    {"minimum_minor", static_cast<std::int64_t>(
                                          extension.minimum_minor)}});
            }
            root.insert("extensions", std::move(extensions));
            std::ostringstream stream;
            stream << root << '\n';
            return std::move(stream).str();
        }

        [[nodiscard]] std::string makeCompositionSource(
            const ProjectBuildUsage& usage,
            const std::set<ESystemRecipe>& recipes)
        {
            const bool renderer = !usage.required_render_features.empty() ||
                !usage.optional_render_features.empty();
            std::ostringstream source;
            addIncludes(source, recipes, renderer);
            source <<
                "namespace lux::generated\n"
                "{\n"
                "    [[nodiscard]] bool installGameSystems(\n"
                "        lux::ecs::ScheduleBuilder& builder,\n"
                "        const lux::ecs::ComponentTypeCatalog& components";
            addSystemParameters(source, recipes);
            source << ")\n    {\n";
            if (recipes.empty())
            {
                source <<
                    "        (void)builder;\n"
                    "        (void)components;\n"
                    "        return true;\n";
            }
            else
            {
                source << "        return\n";
                for (auto iterator = recipes.begin();
                     iterator != recipes.end(); ++iterator)
                {
                    addSystemCall(source, *iterator);
                    source << (std::next(iterator) == recipes.end()
                        ? ";\n"
                        : " &&\n");
                }
            }
            source << "    }\n";
            if (renderer)
            {
                source <<
                    "\n    void installGameRenderer(\n"
                    "        lux::render::GeneralRenderServer& server,\n"
                    "        lux::render::FeatureCatalog& catalog,\n"
                    "        std::vector<lux::render::FeatureAttach>& plan)\n"
                    "    {\n"
                    "        lux::runtime::registerStandardRenderFeatures(\n"
                    "            server,\n"
                    "            catalog,\n"
                    "            plan\n"
                    "        );\n"
                    "    }\n";
            }
            source << "}\n";
            return std::move(source).str();
        }
    }

    lux::cxx::expected<void, GameExportFailure> mergeSceneUsage(
        ProjectBuildUsage& usage,
        const lux::scene::SceneDescription& scene) noexcept
    {
        usage.required_components.insert(
            usage.required_components.end(),
            scene.required_components.begin(),
            scene.required_components.end());
        usage.required_render_features.insert(
            usage.required_render_features.end(),
            scene.required_render_features.begin(),
            scene.required_render_features.end());
        usage.optional_render_features.insert(
            usage.optional_render_features.end(),
            scene.optional_render_features.begin(),
            scene.optional_render_features.end());
        usage.scene_required_extensions.insert(
            usage.scene_required_extensions.end(),
            scene.required_extensions.begin(),
            scene.required_extensions.end());
        usage.spatial3d_streaming = usage.spatial3d_streaming ||
            !scene.spatial3d_catalog.empty();
        return {};
    }

    lux::cxx::expected<GameBuildArtifacts, GameExportFailure>
    writeGameBuildArtifacts(
        ProjectBuildUsage usage,
        const std::filesystem::path& output_directory) noexcept
    {
        if (usage.binary_name.empty() || output_directory.empty())
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::INVALID_ARGUMENT,
                "game build usage requires a binary name and output directory"));
        }
        if (auto canonical = canonicalize(usage); !canonical)
            return lux::cxx::unexpected(std::move(canonical.error()));

        std::error_code error;
        const auto build_root = output_directory / "build";
        std::filesystem::create_directories(build_root, error);
        if (error)
        {
            return lux::cxx::unexpected(failure(
                EGameExportError::FILESYSTEM_ERROR,
                "cannot create game build-graph directory '" +
                    build_root.string() + "': " + error.message()));
        }
        const auto recipes = deriveSystemRecipes(usage);
        const auto usage_path = build_root / kUsageManifestName;
        const auto composition_path = build_root / kCompositionSourceName;
        if (auto written = writeText(
                usage_path,
                makeUsageManifest(usage, recipes)); !written)
        {
            return lux::cxx::unexpected(std::move(written.error()));
        }
        if (auto written = writeText(
                composition_path,
                makeCompositionSource(usage, recipes)); !written)
        {
            return lux::cxx::unexpected(std::move(written.error()));
        }
        return GameBuildArtifacts{usage_path, composition_path};
    }
}
