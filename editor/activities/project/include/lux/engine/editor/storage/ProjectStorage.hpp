#pragma once

#include <lux/engine/editor/storage/visibility.h>

#include <filesystem>
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/project/ProjectCatalogModel.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <lux/engine/editor/storage/ProjectOpenData.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <unordered_map>

namespace lux::editor
{
    class LUX_EDITOR_STORAGE_PUBLIC ProjectStorage final : public lux::object::LuxObject
    {
    public:
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
        // Author source bytes, distinct from cooked assets. The captured file digest is checked on every read.
        [[nodiscard]] EditorResult<asset::AssetVfsView> captureSource(
            asset::AssetId,
            std::size_t max_bytes,
            std::string expected_digest
        ) const noexcept;
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
            return catalog_.entries();
        }
        [[nodiscard]] std::uint64_t catalogRevision() const noexcept
        {
            return catalog_.revision();
        }
        [[nodiscard]] project::ProjectCatalogModel& catalogModel() noexcept
        {
            return catalog_;
        }
        [[nodiscard]] const project::ProjectCatalogModel& catalogModel() const noexcept
        {
            return catalog_;
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
        bool closing_{};
        project::ProjectCatalogModel catalog_;
    };
} // namespace lux::editor
