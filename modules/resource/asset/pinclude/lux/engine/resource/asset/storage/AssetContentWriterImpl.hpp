#pragma once

#include <lux/engine/resource/asset/storage/AssetContentWriter.hpp>
#include <lux/engine/platform/FileHandle.hpp>

namespace lux::asset
{
    struct AssetContentWriter::Impl final
    {
        std::shared_ptr<IAssetProvider> provider;
        const AssetReadSource* base{};
        AssetId id;
        EAssetContentWrite mode{};
        std::filesystem::path root_path;
        std::filesystem::path content_path;
        std::string content_name;
        platform::FileHandle content;
        EAssetPublication state{EAssetPublication::UNPUBLISHED};
    };
} // namespace lux::asset
