#pragma once

#include <lux/engine/editor/core/visibility.h>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lux::editor
{
    enum class EProjectAssetKind : std::uint8_t
    {
        SCENE,
        MATERIAL_GRAPH,
        FLOW_GRAPH,
        MODEL,
        TEXTURE
    };

    struct ProjectAssetEntry final
    {
        asset::AssetId id;
        EProjectAssetKind kind{};
        std::string source_path;
        std::string cooked_path;
        // SHA-256 of source bytes. A source save does not imply a successful/current compiled artifact.
        std::string source_digest;
        std::string compiled_source_digest;
        // Stable project-relative browser directory, independent of immutable compiled revision storage.
        std::string mount_path;
        friend bool operator==(const ProjectAssetEntry &, const ProjectAssetEntry &) = default;
    };

    struct ProjectManifest final
    {
        asset::AssetId id;
        std::string name;
        std::string default_scene;
        std::vector<ProjectAssetEntry> assets;
        friend bool operator==(const ProjectManifest &, const ProjectManifest &) = default;
    };
    enum class EProjectManifestError : std::uint8_t
    {
        INVALID_ARGUMENT,
        LIMIT_EXCEEDED,
        PARSE_FAILURE,
        UNSUPPORTED_FORMAT,
        UNKNOWN_FIELD,
        MISSING_FIELD,
        INVALID_IDENTITY,
        INVALID_PATH,
        INVALID_DIGEST,
        UNKNOWN_ASSET_KIND,
        DUPLICATE_IDENTITY,
        DUPLICATE_PATH,
        INVALID_DEFAULT_SCENE
    };

    struct ProjectManifestFailure final
    {
        EProjectManifestError code{};
        std::string field;
        std::size_t asset{};
        std::uint32_t line{}, column{};
    };

    struct ProjectManifestLimits final
    {
        std::size_t max_bytes{16U * 1024U * 1024U};
        std::size_t max_assets{100000};
        std::size_t max_path_bytes{1024};
    };
    template <class T> using ProjectManifestResult = lux::cxx::expected<T, ProjectManifestFailure>;

    [[nodiscard]] LUX_EDITOR_CORE_PUBLIC ProjectManifestResult<void> validateProjectManifest(
        const ProjectManifest &, ProjectManifestLimits limits = {}) noexcept;
    [[nodiscard]] LUX_EDITOR_CORE_PUBLIC ProjectManifestResult<ProjectManifest> decodeProjectManifest(
        std::string_view utf8, ProjectManifestLimits limits = {}) noexcept;
    [[nodiscard]] LUX_EDITOR_CORE_PUBLIC ProjectManifestResult<std::string> encodeProjectManifest(
        const ProjectManifest &, ProjectManifestLimits limits = {}) noexcept;

    // Portable paths are project-relative POSIX paths. Native resolution and all I/O belong to Process work.
    [[nodiscard]] LUX_EDITOR_CORE_PUBLIC bool validProjectPath(std::string_view value) noexcept;
} // namespace lux::editor
