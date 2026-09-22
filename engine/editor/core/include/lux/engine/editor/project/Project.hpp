#pragma once

#include <filesystem>
#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/engine/editor/project/AssetCatalog.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <lux/engine/editor/project/ProjectPublication.hpp>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <unordered_map>

namespace lux::editor
{
struct ProjectSource final
{
    ProjectManifest manifest;
    std::filesystem::path file;
    std::vector<ProjectPackage> mounts;
    ProjectWriteLease write_lease;
    std::string manifest_digest;
    std::vector<std::pair<std::string, std::string>> source_digests;
};

// Blocking source preparation, invoked through Process before Project adoption.
[[nodiscard]] LUX_EDITOR_CORE_PUBLIC EditorResult<ProjectSource> readProjectSource(const std::filesystem::path &);

class LUX_EDITOR_CORE_PUBLIC LUX_OBJECT() Project final : public object::Object<Project>
{
  public:
    static const signal_type<std::uint64_t> catalogChanged;
    static const signal_type<asset::AssetId> assetContentChanged;
    [[nodiscard]] static EditorResult<std::unique_ptr<Project>> open(ProjectSource &, process::BlockingScheduler,
                                                                     process::TaskScope &, object::ObjectDispatcherRef);
    ~Project() override;

    [[nodiscard]] const ProjectManifest &manifest() const noexcept
    {
        return source_.manifest;
    }

    [[nodiscard]] const std::filesystem::path &root() const noexcept
    {
        return root_;
    }

    [[nodiscard]] const ProjectAssetEntry *asset(asset::AssetId) const noexcept;
    [[nodiscard]] process::asset_loading::AssetReadPort assetReads() const noexcept;
    [[nodiscard]] EditorResult<process::asset_loading::AssetReadPort> captureAssetReads();
    [[nodiscard]] process::TaskScope &tasks() noexcept
    {
        return tasks_;
    }

    [[nodiscard]] asset::AssetVfsView assets() const noexcept
    {
        return vfs_.view();
    }

    [[nodiscard]] std::string_view assetName(asset::AssetId) const noexcept;
    [[nodiscard]] std::span<const AssetCatalogEntry> catalog() const noexcept
    {
        return catalog_;
    }
    [[nodiscard]] std::uint64_t catalogRevision() const noexcept
    {
        return catalog_revision_;
    }
    [[nodiscard]] const AssetCatalogEntry *catalogAsset(asset::AssetId) const noexcept;
    [[nodiscard]] AssetReference reference(asset::AssetId) const noexcept;
    [[nodiscard]] EditorResult<asset::AssetId> resolveReference(AssetReference, std::uint32_t required_magic) const;
    [[nodiscard]] bool writable() const noexcept
    {
        return source_.write_lease.writable();
    }
    [[nodiscard]] std::string_view sourceDigest(std::string_view path) const noexcept;
    [[nodiscard]] EditorResult<ProjectPublication> preparePublication(ProjectUpdate &);
    [[nodiscard]] EditorResult<void> adoptPublication(ProjectPublication &, ProjectPublicationReceipt &);
    void requestClose() noexcept;
    [[nodiscard]] EditorResult<bool> advanceClose();

  private:
    friend struct ProjectPublication;
    Project(object::ObjectDispatcherRef, std::uint64_t instance, process::TaskScope &, process::BlockingScheduler);
    void rebuildCatalog();
    process::TaskScope &tasks_;
    process::BlockingScheduler blocking_;
    std::vector<std::weak_ptr<process::asset_loading::VfsAssetReadEndpoint>> read_snapshots_;
    ProjectSource source_;
    std::filesystem::path root_;
    asset::AssetVfs vfs_;
    struct MountedPackage final
    {
        std::string path;
        asset::MountId id;
        std::vector<asset::ProviderEntry> entries;
    };
    std::vector<MountedPackage> mounts_;
    std::shared_ptr<process::asset_loading::VfsAssetReadEndpoint> reads_;
    bool publishing_{};
    std::uint64_t instance_{};
    std::uint64_t catalog_revision_{};
    std::vector<AssetCatalogEntry> catalog_;
    std::unordered_map<asset::AssetId, std::size_t> catalog_by_id_;
};
} // namespace lux::editor
