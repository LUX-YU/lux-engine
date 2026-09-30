#pragma once

#include <lux/engine/editor/storage/visibility.h>

#include <filesystem>
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/project/AssetCatalog.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <lux/engine/editor/storage/ProjectOpenData.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <unordered_map>

namespace lux::editor::detail
{
    class ProjectWrite;
}

namespace lux::editor
{
    class LUX_EDITOR_STORAGE_PUBLIC ProjectStorage final : public lux::object::LuxObject
    {
    public:
        object::TSignal<std::uint64_t> catalogChanged{*this};
        object::TSignal<asset::AssetId> assetContentChanged{*this};
        // Storage waiters only schedule collection work; publication never reenters business callbacks.
        void whenPublicationAvailable(process::CompletionWork::Request);
        void dispatchEvents() noexcept;
        [[nodiscard]] static EditorResult<std::unique_ptr<ProjectStorage>> open(
            ProjectOpenData&,
            asset::AssetVfs&,
            process::BlockingScheduler,
            process::TaskScope&,
            object::ObjectDispatcherRef
        );
        ~ProjectStorage() override;

        [[nodiscard]] const ProjectManifest& manifest() const noexcept
        {
            return source_.manifest;
        }

        [[nodiscard]] const std::filesystem::path& projectFile() const noexcept
        {
            return source_.file;
        }

        [[nodiscard]] const std::filesystem::path& root() const noexcept
        {
            return root_;
        }

        [[nodiscard]] const ProjectAssetEntry* asset(asset::AssetId) const noexcept;
        [[nodiscard]] process::asset_loading::AssetReadPort assetReads() const noexcept;
        [[nodiscard]] EditorResult<process::asset_loading::AssetReadPort> captureAssetReads();
        // Author source bytes, distinct from the immutable cooked asset read port.
        [[nodiscard]] EditorResult<asset::AssetVfsView> captureSource(asset::AssetId, std::size_t max_bytes)
            const noexcept;
        [[nodiscard]] process::TaskScope& tasks() noexcept
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
        [[nodiscard]] const AssetCatalogEntry* catalogAsset(asset::AssetId) const noexcept;
        [[nodiscard]] AssetReference reference(asset::AssetId) const noexcept;
        [[nodiscard]] EditorResult<asset::AssetId> resolveReference(AssetReference, std::uint32_t required_magic) const;
        [[nodiscard]] bool writable() const noexcept
        {
            return source_.write_lease.writable();
        }
        [[nodiscard]] std::string_view sourceDigest(std::string_view path) const noexcept;
        [[nodiscard]] EditorResult<ProjectPublication> preparePublication(ProjectUpdate&);
        [[nodiscard]] EditorResult<void> adoptPublication(ProjectPublication&, ProjectPublicationReceipt&);
        [[nodiscard]] EditorResult<void> savePlugins(std::vector<ProjectPluginEntry>, process::ExecutionRuntime&);
        [[nodiscard]] const VPublicationStatus* pluginSaveStatus() const noexcept;
        [[nodiscard]] EditorResult<void> retryPluginSave();
        [[nodiscard]] EditorResult<void> acknowledgePluginSave();
        void abandonPluginSave() noexcept;
        void requestClose() noexcept;
        [[nodiscard]] EditorResult<bool> advanceClose();

    private:
        friend struct ProjectPublication;
        ProjectStorage(
            object::ObjectDispatcherRef,
            asset::AssetVfs&,
            std::uint64_t instance,
            process::TaskScope&,
            process::BlockingScheduler
        );
        void rebuildCatalog();
        process::TaskScope& tasks_;
        process::BlockingScheduler blocking_;
        std::vector<std::weak_ptr<process::asset_loading::VfsAssetReadEndpoint>> read_snapshots_;
        ProjectOpenData source_;
        std::filesystem::path root_;
        asset::AssetVfs& vfs_;
        struct MountedPackage final
        {
            std::string path;
            asset::MountId id;
            std::vector<asset::ProviderEntry> entries;
        };
        std::vector<MountedPackage> mounts_;
        std::shared_ptr<process::asset_loading::VfsAssetReadEndpoint> reads_;
        bool publishing_{};
        std::vector<process::CompletionWork::Request> publication_waiters_;
        std::vector<asset::AssetId> changed_assets_;
        std::uint64_t notified_catalog_revision_{};
        bool closing_{};
        std::unique_ptr<detail::ProjectWrite> plugin_save_;
        std::uint64_t instance_{};
        std::uint64_t catalog_revision_{};
        std::vector<AssetCatalogEntry> catalog_;
        std::unordered_map<asset::AssetId, std::size_t> catalog_by_id_;
    };
} // namespace lux::editor
