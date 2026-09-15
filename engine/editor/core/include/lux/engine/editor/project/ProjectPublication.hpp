#pragma once

#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <filesystem>
#include <stop_token>

namespace lux::editor
{
    class Project;
    enum class EProjectPublicationError : std::uint8_t
    {
        INVALID_PATH,
        CONFLICT,
        READ,
        WRITE,
        FLUSH,
        REPLACE,
        JOURNAL,
        RECOVERY_CONFLICT,
        CANCELLED
    };

    struct ProjectPublicationFailure final
    {
        EProjectPublicationError code;
        std::filesystem::path path;
        std::uint64_t platform_code{};
        std::size_t published_files{};
    };

    // The kernel releases this lease on normal exit and process interruption alike.
    class LUX_EDITOR_CORE_PUBLIC ProjectWriteLease final
    {
      public:
        ProjectWriteLease() noexcept = default;
        ~ProjectWriteLease();
        ProjectWriteLease(ProjectWriteLease&&) noexcept;
        ProjectWriteLease& operator=(ProjectWriteLease&&) noexcept;
        ProjectWriteLease(const ProjectWriteLease&) = delete;
        ProjectWriteLease& operator=(const ProjectWriteLease&) = delete;

        [[nodiscard]] static EditorResult<ProjectWriteLease> acquire(const std::filesystem::path& project_root);
        [[nodiscard]] bool writable() const noexcept { return handle_ != -1; }

      private:
        explicit ProjectWriteLease(std::intptr_t handle) noexcept : handle_(handle) {}
        std::intptr_t handle_{-1};
    };

    struct ProjectFileChange final
    {
        std::string path;
        std::string before_digest; // "missing" authorizes creation, never replacement of an existing file.
        lux::cxx::SharedBytes<> bytes;
        bool reuse_identical{}; // An existing identical immutable blob may be reused; never overwritten.
    };

    // Prepared on Blocking; the index rows avoid provider I/O while Main publishes its catalog.
    struct ProjectPackage final
    {
        std::string path;
        asset::MountDesc mount;
        std::vector<asset::ProviderEntry> entries;
    };

    [[nodiscard]] LUX_EDITOR_CORE_PUBLIC EditorResult<ProjectPackage>
    readProjectPackage(const std::filesystem::path& root, std::string path);

    struct ProjectUpdate final
    {
        std::vector<ProjectAssetEntry> assets;
        std::vector<lux::asset::AssetId> removed;
        std::vector<ProjectFileChange> files;
    };

    struct LUX_EDITOR_CORE_PUBLIC ProjectPublication final
    {
        ProjectPublication() = default;
        ~ProjectPublication();
        ProjectPublication(ProjectPublication&&) noexcept;
        ProjectPublication& operator=(ProjectPublication&&) noexcept;
        ProjectPublication(const ProjectPublication&) = delete;
        ProjectPublication& operator=(const ProjectPublication&) = delete;

        std::filesystem::path root;
        std::string manifest_path;
        std::string before_manifest_digest;
        ProjectManifest manifest;
        std::vector<ProjectFileChange> files;
        std::vector<std::string> package_paths;

      private:
        friend class Project;
        Project* owner_{}; // The reservation lives on the Project owner lane; workers only borrow its data.
    };

    struct ProjectPublicationReceipt final
    {
        ProjectManifest manifest;
        std::string manifest_digest;
        std::size_t published_files{};
        EditorResult<void> cleanup;
        std::vector<std::pair<std::string, std::string>> file_digests;
        std::vector<ProjectPackage> packages;
    };

    // All three functions run on Blocking. Publication never adopts live Project state.
    [[nodiscard]] LUX_EDITOR_CORE_PUBLIC std::string projectContentDigest(std::span<const std::byte>);
    [[nodiscard]] LUX_EDITOR_CORE_PUBLIC EditorResult<std::string> projectFileDigest(const std::filesystem::path&);
    [[nodiscard]] LUX_EDITOR_CORE_PUBLIC EditorResult<ProjectPublicationReceipt>
    publishProjectFiles(const ProjectPublication&, std::stop_token = {});
    [[nodiscard]] LUX_EDITOR_CORE_PUBLIC EditorResult<void> recoverProjectFiles(const std::filesystem::path& root);
}
