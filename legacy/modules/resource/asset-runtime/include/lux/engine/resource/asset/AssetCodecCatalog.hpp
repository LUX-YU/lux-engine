#pragma once
/**
 * @file AssetCodecCatalog.hpp
 * @brief Immutable, product-composed asset codec table.
 *
 * The resource layer owns the stable common EAssetType range, while Engine
 * products may reserve additional numeric values and decide which codecs are
 * present. A catalog is assembled once,
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
        MISSING_PRIMARY_MAGIC,
        DUPLICATE_ASSET_TYPE,
        DUPLICATE_MAGIC,
        DUPLICATE_CPP_TYPE,
        TYPE_HASH_COLLISION
    };

    using AssetShellCreateFn = std::unique_ptr<LuxAsset> (*)(
        std::unique_ptr<AssetInfo>
    ) noexcept;

    using AssetImageShellCreateFn = lux::cxx::expected<
        std::unique_ptr<LuxAsset>,
        EAssetError> (*)(std::span<const std::byte>) noexcept;

    struct AssetCodecDescriptor final
    {
        EAssetType              type{EAssetType::UNKNOWN};
        std::uint64_t           cpp_type_hash{0u};
        std::string             cpp_type_name;
        EAssetShippingClass     shipping{EAssetShippingClass::RUNTIME};
        AssetSerDeserFactoryFn  create{};
        AssetShellCreateFn      create_shell{};
        std::uint32_t           primary_magic{0u};
        std::uint32_t           legacy_magic{0u};
        AssetImageShellCreateFn create_shell_from_image{};

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

        [[nodiscard]] const AssetCodecDescriptor* findByMagic(
            std::uint32_t magic) const noexcept;

        [[nodiscard]] std::span<const AssetCodecDescriptor>
        descriptors() const noexcept
        {
            return descriptors_;
        }

        [[nodiscard]] std::unique_ptr<AssetSerDeser> create(
            EAssetType type,
            std::shared_ptr<AssetManager> owner
        ) const;

        /// Decode a complete image through the selected manager-less
        /// SerDeser. The returned asset is owning and not registered.
        [[nodiscard]] lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        decodeAsset(lux::cxx::SharedBytes<> image) const noexcept;

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
