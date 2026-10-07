#pragma once

#include <filesystem>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace lux::editor
{
    // Persistent identities are independent of an open Context or runtime Scene instance.
    struct ProjectManifest final
    {
        struct PluginRecord final
        {
            std::string id;
            std::uint32_t version{};
            friend bool operator==(const PluginRecord&, const PluginRecord&) = default;
        };
        struct SceneRecord final
        {
            asset::AssetId id;
            std::string name;
            std::string path;
            std::string profile;
            friend bool operator==(const SceneRecord&, const SceneRecord&) = default;
        };
        std::uint32_t format_version{1};
        uuids::uuid id;
        std::string name;
        std::vector<PluginRecord> plugins;
        std::vector<SceneRecord> scenes;
        std::optional<asset::AssetId> startup_scene;
        friend bool operator==(const ProjectManifest&, const ProjectManifest&) = default;
    };

    enum class EProjectError : std::uint8_t
    {
        INVALID_FORMAT,
        UNSUPPORTED_VERSION,
        INVALID_IDENTITY,
        INVALID_NAME,
        INVALID_PLUGIN,
        INVALID_PROFILE,
        INVALID_PATH,
        DUPLICATE_IDENTITY,
        DUPLICATE_PATH,
        INVALID_STARTUP_SCENE,
        LIMIT,
        CANCELLED,
        IO,
        DESTINATION_EXISTS,
        PUBLICATION_UNKNOWN
    };
    struct ProjectFailure final
    {
        EProjectError code{};
        std::size_t ordinal{};
        std::error_code system;
    };
    template <class T> using ProjectResult = cxx::expected<T, ProjectFailure>;

    // Canonical identifiers contain lower-case ASCII segments separated by dots.
    [[nodiscard]] bool isCanonicalProjectName(std::string_view) noexcept;
    [[nodiscard]] ProjectResult<void> validateProjectManifest(const ProjectManifest&) noexcept;
    [[nodiscard]] ProjectResult<ProjectManifest> decodeProjectManifest(std::string_view) noexcept;
    [[nodiscard]] ProjectResult<std::string> encodeProjectManifest(const ProjectManifest&) noexcept;
    [[nodiscard]] ProjectResult<ProjectManifest> readProjectManifest(
        const std::filesystem::path&,
        std::stop_token = {}
    ) noexcept;

    enum class EProjectWrite : std::uint8_t
    {
        CREATE,
        REPLACE
    };
    // Single-file publication. Cancellation is observed before publication, never after it.
    // PUBLICATION_UNKNOWN means the rename happened but directory durability was not confirmed.
    [[nodiscard]] ProjectResult<void> writeProjectManifestAtomic(
        const std::filesystem::path&,
        const ProjectManifest&,
        EProjectWrite,
        std::stop_token = {}
    ) noexcept;
} // namespace lux::editor
