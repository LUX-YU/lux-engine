#pragma once

#include <lux/engine/editor/storage/ProjectPublication.hpp>

namespace lux::editor
{
    class PreparedProjectOpen final
    {
    public:
        PreparedProjectOpen(PreparedProjectOpen&& other) noexcept : PreparedProjectOpen() { swap(other); }
        PreparedProjectOpen& operator=(PreparedProjectOpen&& other) noexcept
        {
            if (this != &other)
            {
                PreparedProjectOpen released(std::move(*this));
                swap(other);
            }
            return *this;
        }
        PreparedProjectOpen(const PreparedProjectOpen&) = delete;
        PreparedProjectOpen& operator=(const PreparedProjectOpen&) = delete;
        [[nodiscard]] const ProjectManifest& manifest() const noexcept { return manifest_; }
        [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

    private:
        PreparedProjectOpen() = default;
        void swap(PreparedProjectOpen& other) noexcept
        {
            std::swap(write_lease_, other.write_lease_);
            std::swap(manifest_, other.manifest_);
            file_.swap(other.file_);
            mounts_.swap(other.mounts_);
            manifest_digest_.swap(other.manifest_digest_);
            source_digests_.swap(other.source_digests_);
        }
        friend class ProjectStorage;
        friend EditorResult<PreparedProjectOpen> prepareProjectOpen(const std::filesystem::path&);
        ProjectWriteLease write_lease_;
        ProjectManifest manifest_;
        std::filesystem::path file_;
        std::vector<ProjectPackage> mounts_;
        std::string manifest_digest_;
        std::vector<std::pair<std::string, std::string>> source_digests_;
    };

    [[nodiscard]] LUX_EDITOR_STORAGE_PUBLIC EditorResult<PreparedProjectOpen>
    prepareProjectOpen(const std::filesystem::path&);
}
