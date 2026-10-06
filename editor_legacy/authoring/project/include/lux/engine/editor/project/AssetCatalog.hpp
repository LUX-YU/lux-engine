#pragma once

#include <lux/engine/editor/project/ProjectManifest.hpp>

namespace lux::editor
{
    struct AssetCatalogEntry final
    {
        asset::AssetId id;
        asset::AssetId source;
        std::uint32_t magic{};
        std::string path;
    };

    // A drag/picker refers to one Project instance and one immutable catalog revision.
    // Asset identity alone cannot authorize a drop from a closed or foreign project.
    struct AssetReference final
    {
        std::uint64_t project_instance{};
        std::uint64_t catalog_revision{};
        asset::AssetId asset;
    };

    enum class EAssetReferenceError : std::uint8_t
    {
        FOREIGN_PROJECT,
        STALE_CATALOG,
        MISSING_ASSET,
        WRONG_TYPE
    };
}
