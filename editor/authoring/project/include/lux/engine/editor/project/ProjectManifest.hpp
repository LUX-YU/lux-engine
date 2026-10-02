#pragma once

#include <lux/engine/editor/project/visibility.h>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/resource/asset/AssetTypeId.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lux::editor
{
    struct ProjectAssetEntry final
    {
        asset::AssetId id;
        std::string source_type; // Canonical source format name; not the cooked asset type.
        std::string source_path;
        std::string cooked_path;
        // SHA-256 of source bytes. A source save does not imply a successful/current compiled artifact.
        std::string source_digest;
        std::string compiled_source_digest;
        // Stable project-relative browser directory, independent of immutable compiled revision storage.
        std::string mount_path;
        std::uint32_t source_version{1};
        [[nodiscard]] asset::AssetTypeId sourceType() const noexcept
        {
            return asset::AssetTypeId::fromName(source_type);
        }
        friend bool operator==(const ProjectAssetEntry&, const ProjectAssetEntry&) = default;
    };

    struct ProjectPluginEntry final
    {
        std::string id;
        std::uint32_t version{};
        std::string description_path;
        friend bool operator==(const ProjectPluginEntry&, const ProjectPluginEntry&) = default;
    };

    struct ProjectManifest final
    {
        asset::AssetId id;
        std::string name;
        std::string default_scene;
        std::vector<ProjectAssetEntry> assets;
        std::vector<ProjectPluginEntry> plugins;
        friend bool operator==(const ProjectManifest&, const ProjectManifest&) = default;
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
        UNKNOWN_ASSET_KIND, // Only the finite v1/v2 read-only mapping.
        INVALID_SOURCE_TYPE,
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
        std::size_t max_plugins{1024};
    };
    template <class T> using ProjectManifestResult = lux::cxx::expected<T, ProjectManifestFailure>;

    [[nodiscard]] LUX_EDITOR_PROJECT_PUBLIC ProjectManifestResult<void> validateProjectManifest(
        const ProjectManifest&,
        ProjectManifestLimits limits = {}
    ) noexcept;
    [[nodiscard]] LUX_EDITOR_PROJECT_PUBLIC ProjectManifestResult<ProjectManifest> decodeProjectManifest(
        std::string_view utf8,
        ProjectManifestLimits limits = {}
    ) noexcept;
    [[nodiscard]] LUX_EDITOR_PROJECT_PUBLIC ProjectManifestResult<std::string> encodeProjectManifest(
        const ProjectManifest&,
        ProjectManifestLimits limits = {}
    ) noexcept;

    // Portable paths are project-relative POSIX paths. Native resolution and all I/O belong to Process work.
    [[nodiscard]] LUX_EDITOR_PROJECT_PUBLIC bool validProjectPath(std::string_view value) noexcept;
} // namespace lux::editor
