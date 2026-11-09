#pragma once

#include <lux/engine/resource/asset/storage/AssetProvider.hpp>

namespace lux::asset
{
    // Index of cooked .luxasset files under one physical directory. Startup and refresh read
    // only fixed headers; open/replace perform actual IO and should run on a blocking scheduler.
    class LUX_ASSET_PUBLIC DirectoryAssetProvider final : public IAssetProvider
    {
    public:
        using CreateResult = cxx::expected<std::shared_ptr<DirectoryAssetProvider>, EAssetStorageError>;

        [[nodiscard]] static CreateResult create(const std::filesystem::path& root) noexcept;
        ~DirectoryAssetProvider() override;

        [[nodiscard]] std::optional<AssetId> resolve(std::string_view path) const override;
        [[nodiscard]] bool contains(const AssetId& id) const override;
        [[nodiscard]] cxx::expected<ProviderEntry, EAssetStorageError> describe(AssetId id) const override;
        [[nodiscard]] cxx::expected<AssetBlob, EAssetStorageError>
        open(const AssetId& id, std::size_t max_bytes = SIZE_MAX) const override;

        void enumerate(const std::function<void(const ProviderEntry&)>& fn) const override;
        [[nodiscard]] std::optional<std::string> pathOf(const AssetId& id) const override;

        [[nodiscard]] cxx::expected<void, EAssetStorageError>
        replace(AssetId id, std::span<const std::byte> bytes) override;

        [[nodiscard]] cxx::expected<std::vector<AssetId>, EAssetStorageError>
        refresh(std::span<const std::filesystem::path> paths) override;

    protected:
        [[nodiscard]] cxx::expected<AssetReadSource, EAssetStorageError> openSource(AssetId id) noexcept override;

        [[nodiscard]] cxx::expected<AssetContentWriter, EAssetStorageError>
        beginContentWrite(
            AssetId id, const AssetReadSource* base, EAssetContentWrite mode, std::string_view new_path
        ) noexcept override;
        [[nodiscard]] cxx::expected<void, EAssetStorageError>
        publishContentRoot(
            AssetId id, const AssetReadSource* base, const std::filesystem::path& destination,
            std::span<const std::byte> root
        ) noexcept override;

    private:
        struct Impl;
        explicit DirectoryAssetProvider(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
