#pragma once

#include <lux/engine/toolchain/game_export/GameExporter.hpp>

#include <lux/engine/scene/SceneDescription.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lux::toolchain::detail
{
    struct BuildExtension final
    {
        lux::extensions::ExtensionId id;
        std::filesystem::path source;
        std::uint16_t required_major{0u};
        std::uint16_t minimum_minor{0u};
    };

    /**
     * Ephemeral input to the game build graph.
     *
     * This value is owned by Cook and is deliberately absent from Runtime.
     * It is neither installed nor serialized beside LaunchManifest.
     */
    struct ProjectBuildUsage final
    {
        std::string binary_name;
        std::vector<lux::ecs::scene_format::RequiredComponentSchema>
            required_components;
        std::vector<std::string> required_render_features;
        std::vector<std::string> optional_render_features;
        std::vector<lux::scene::RequiredExtension>
            scene_required_extensions;
        std::vector<BuildExtension> selected_extensions;
        bool spatial3d_streaming{false};
    };

    struct GameBuildArtifacts final
    {
        std::filesystem::path usage_manifest;
        std::filesystem::path composition_source;
    };

    [[nodiscard]] lux::cxx::expected<void, GameExportFailure>
    mergeSceneUsage(
        ProjectBuildUsage& usage,
        const lux::scene::SceneDescription& scene) noexcept;

    [[nodiscard]] lux::cxx::expected<GameBuildArtifacts, GameExportFailure>
    writeGameBuildArtifacts(
        ProjectBuildUsage usage,
        const std::filesystem::path& output_directory) noexcept;
}
