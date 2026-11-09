#pragma once

#include <lux/engine/resource/asset/storage/AssetReadSource.hpp>
#include <lux/engine/platform/FileHandle.hpp>

namespace lux::asset
{
    struct AssetReadSource::Impl final
    {
        std::shared_ptr<IAssetProvider> provider;
        platform::FileHandle root;
        platform::FileHandle content;
        std::filesystem::path root_path;
        std::filesystem::path content_path;
        cxx::algorithm::Sha256Digest root_digest;
        std::uint64_t root_size{};
        std::uint64_t content_size{};
    };
} // namespace lux::asset
