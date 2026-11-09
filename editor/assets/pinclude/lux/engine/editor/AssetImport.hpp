#pragma once

#include <exec/task.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

namespace lux::editor::detail
{
    struct AssetCatalog final
    {
        std::uint64_t revision{};
        std::vector<ProjectAsset> records;
    };

    struct ImportedAssets final
    {
        AssetCatalog catalog;
        std::vector<asset::MountDesc> mounts;
        std::vector<asset::AssetId> roots;
        bool complete_catalog{}; // Open/reload replaces all mounts; import replaces only the returned roots.
    };

    // Finite pipelines own their arguments. They never borrow EditorContext or ProjectAssets.
    [[nodiscard]] exec::task<AssetImportResult<ImportedAssets>> openAssetCatalog(
        execution::ExecutionRuntime&,
        std::filesystem::path root,
        execution::TaskReporter
    ) noexcept;
    [[nodiscard]] exec::task<AssetImportResult<ImportedAssets>> importAsset(
        execution::ExecutionRuntime&,
        std::filesystem::path root,
        AssetCatalog expected,
        ProjectAsset candidate,
        AssetImportCook cook,
        execution::TaskReporter
    ) noexcept;
} // namespace lux::editor::detail
