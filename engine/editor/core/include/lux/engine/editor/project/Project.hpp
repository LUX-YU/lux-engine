#pragma once

#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <filesystem>

namespace lux::editor
{
    struct ProjectSource final
    {
        ProjectManifest manifest;
        std::filesystem::path file;
        std::vector<asset::MountDesc> mounts;
    };

    // Blocking source preparation, invoked through Process before Project adoption.
    [[nodiscard]] LUX_EDITOR_CORE_PUBLIC EditorResult<ProjectSource> readProjectSource(const std::filesystem::path &);

    class LUX_EDITOR_CORE_PUBLIC Project final : public object::Object<Project>
    {
      public:
        [[nodiscard]] static EditorResult<std::unique_ptr<Project>> open(ProjectSource &, process::BlockingScheduler,
                                                                         object::ObjectDispatcherRef);
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

        [[nodiscard]] asset::AssetVfsView assets() const noexcept
        {
            return vfs_.view();
        }

        [[nodiscard]] std::string assetName(asset::AssetId) const;
        void requestClose() noexcept;
        [[nodiscard]] EditorResult<bool> advanceClose();

      private:
        explicit Project(object::ObjectDispatcherRef);
        ProjectSource source_;
        std::filesystem::path root_;
        asset::AssetVfs vfs_;
        std::vector<asset::MountId> mounts_;
        std::shared_ptr<process::asset_loading::VfsAssetReadEndpoint> reads_;
    };
} // namespace lux::editor
