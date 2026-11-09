#pragma once

#include <lux/engine/resource/asset/storage/AssetReadSource.hpp>

namespace lux::asset
{
    enum class EAssetContentWrite : std::uint8_t
    {
        CREATE,
        APPEND,
        COMPACT
    };

    enum class EAssetPublication : std::uint8_t
    {
        UNPUBLISHED,
        PUBLISHED,
        UNKNOWN
    };

    // One serial compound write. The caller keeps base alive until this writer is destroyed.
    // Only publish changes the asset root. Destruction never truncates an appended file.
    class LUX_ASSET_PUBLIC AssetContentWriter final
    {
    public:
        ~AssetContentWriter() noexcept;
        AssetContentWriter(AssetContentWriter&& other) noexcept;
        AssetContentWriter& operator=(AssetContentWriter&& other) noexcept;
        AssetContentWriter(const AssetContentWriter&) = delete;
        AssetContentWriter& operator=(const AssetContentWriter&) = delete;

        [[nodiscard]] std::string_view contentName() const noexcept;
        [[nodiscard]] EAssetPublication publication() const noexcept;
        [[nodiscard]] cxx::expected<std::uint64_t, EAssetStorageError>
        append(std::span<const std::byte> bytes) noexcept;
        // Last business operation. UNKNOWN must be reconciled, never treated as an unpublished candidate.
        [[nodiscard]] cxx::expected<void, EAssetStorageError> publish(std::span<const std::byte> root) noexcept;

    private:
        friend class DirectoryAssetProvider;
        friend class AssetVfsView;
        struct Impl;
        explicit AssetContentWriter(std::unique_ptr<Impl> impl) noexcept;
        void attachProvider(std::shared_ptr<IAssetProvider> provider) noexcept;
        [[nodiscard]] cxx::expected<void, EAssetStorageError> createMember() noexcept;
        [[nodiscard]] static cxx::expected<void, EAssetStorageError>
        validateBase(const AssetReadSource& base, const std::filesystem::path& root) noexcept;
        void discardUnpublishedMember() noexcept;
        void retireOldMember(std::span<const std::byte> published_root) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::asset
