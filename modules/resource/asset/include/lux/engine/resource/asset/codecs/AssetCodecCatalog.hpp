#pragma once
/**
 * @file AssetCodecCatalog.hpp
 * @brief Immutable, product-composed asset codec table.
 *
 * The resource layer owns the closed EAssetType vocabulary, while each
 * product decides which codecs are present.  A catalog is assembled once,
 * validated for duplicate/colliding identities, and thereafter shared as an
 * immutable snapshot.  Worker code receives the snapshot by shared ownership;
 * it never consults a mutable global registry.
 */

#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lux::asset
{
    enum class EAssetShippingClass : std::uint8_t
    {
        RUNTIME,
        AUTHORING_ONLY
    };

    enum class EAssetCodecCatalogError : std::uint8_t
    {
        EMPTY_NAME,
        INVALID_TYPE_IDENTITY,
        MISSING_FACTORY,
        DUPLICATE_ASSET_TYPE,
        DUPLICATE_CPP_TYPE,
        TYPE_HASH_COLLISION
    };

    using AssetDataDecodeFn = lux::cxx::expected<AssetDataInjector, EAssetError> (*)(lux::cxx::SharedBytes<>) noexcept;

    using AssetShellCreateFn = std::unique_ptr<LuxAsset> (*)(
        std::unique_ptr<AssetInfo>
    ) noexcept;

    struct AssetCodecDescriptor final
    {
        EAssetType              type{EAssetType::UNKNOWN};
        std::uint64_t           cpp_type_hash{0u};
        std::string             cpp_type_name;
        EAssetShippingClass     shipping{EAssetShippingClass::RUNTIME};
        AssetSerDeserFactoryFn  create{};
        AssetDataDecodeFn       decode{};
        AssetShellCreateFn      create_shell{};

        // Keeps dynamically supplied function pointers alive without making
        // the Resource layer depend on Runtime's concrete ModuleLifetime.
        std::shared_ptr<const void> code_lifetime;
    };

    class LUX_ASSET_PUBLIC AssetCodecCatalog final
    {
    public:
        AssetCodecCatalog() = default;
        AssetCodecCatalog(AssetCodecCatalog&&) noexcept = default;
        AssetCodecCatalog& operator=(AssetCodecCatalog&&) noexcept = default;
        AssetCodecCatalog(const AssetCodecCatalog&) = delete;
        AssetCodecCatalog& operator=(const AssetCodecCatalog&) = delete;

        [[nodiscard]] static lux::cxx::expected<
            AssetCodecCatalog,
            EAssetCodecCatalogError>
        build(std::vector<AssetCodecDescriptor> descriptors) noexcept;

        [[nodiscard]] const AssetCodecDescriptor* find(
            EAssetType type) const noexcept;

        [[nodiscard]] std::span<const AssetCodecDescriptor>
        descriptors() const noexcept
        {
            return descriptors_;
        }

        [[nodiscard]] std::unique_ptr<AssetSerDeser> create(
            EAssetType type,
            std::shared_ptr<AssetManager> owner
        ) const;

        [[nodiscard]] lux::cxx::expected<AssetDataInjector, EAssetError>
        decode(lux::cxx::SharedBytes<> image) const noexcept;

        [[nodiscard]] std::unique_ptr<LuxAsset> createShell(
            std::unique_ptr<AssetInfo> info
        ) const noexcept;

    private:
        explicit AssetCodecCatalog(
            std::vector<AssetCodecDescriptor> descriptors) noexcept
            : descriptors_(std::move(descriptors))
        {}

        std::vector<AssetCodecDescriptor> descriptors_;
    };

    /// Closed cooked-runtime codec snapshot.  The returned shared owner is
    /// process-stable and safe to pass through asynchronous operation state.
    [[nodiscard]] LUX_ASSET_PUBLIC
    std::shared_ptr<const AssetCodecCatalog> runtimeAssetCodecCatalog() noexcept;

    [[nodiscard]] LUX_ASSET_PUBLIC
    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    makeShellFromFile(
        const AssetCodecCatalog& catalog,
        const std::filesystem::path& path
    );

    [[nodiscard]] LUX_ASSET_PUBLIC
    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    makeShellFromMemory(
        const AssetCodecCatalog& catalog,
        const void* bytes,
        std::size_t len
    );
} // namespace lux::asset
