#pragma once

#include <lux/engine/resource/asset/storage/AssetProvider.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>

namespace lux::asset
{
    class AssetContentWriter;
    class DirectoryAssetProvider;
    class AssetVfsView;

    // One physical root and optional content member, retained independently of mount changes.
    // readRoot/bindContent finish before publication; readRange is then safe for concurrent callers.
    class LUX_ASSET_PUBLIC AssetReadSource final
    {
    public:
        ~AssetReadSource() noexcept;
        AssetReadSource(AssetReadSource&& other) noexcept;
        AssetReadSource& operator=(AssetReadSource&& other) noexcept;
        AssetReadSource(const AssetReadSource&) = delete;
        AssetReadSource& operator=(const AssetReadSource&) = delete;

        [[nodiscard]] cxx::expected<AssetBlob, EAssetStorageError> readRoot(std::size_t limit) noexcept;
        [[nodiscard]] cxx::expected<void, EAssetStorageError> bindContent(std::string_view name) noexcept;
        [[nodiscard]] cxx::expected<AssetBlob, EAssetStorageError>
        readRange(std::uint64_t offset, std::uint64_t size, std::size_t limit) const noexcept;
        [[nodiscard]] std::uint64_t contentSize() const noexcept;
        [[nodiscard]] const cxx::algorithm::Sha256Digest& rootDigest() const noexcept;

    private:
        friend class DirectoryAssetProvider;
        friend class AssetVfsView;
        friend class AssetContentWriter;
        struct Impl;
        explicit AssetReadSource(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] static cxx::expected<AssetReadSource, EAssetStorageError>
        openRoot(const std::filesystem::path& path) noexcept;
        void attachProvider(std::shared_ptr<IAssetProvider> provider) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::asset
