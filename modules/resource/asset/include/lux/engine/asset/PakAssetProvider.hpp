#pragma once
// ============================================================================
//  PakAssetProvider — IAssetProvider over an immutable .luxpak container
//  (the shipped-build mount; the editor uses LooseDirProvider).
//
//  The pak index IS the cooked asset registry: resolve() reads the loaded
//  PATH table, contains() binary-searches the uuid-sorted entry table, and
//  open() is a seek+read of the verbatim .luxasset image (+ content-hash
//  verification when the entry carries one). Tombstone entries honor the
//  shadow-delete contract: contains()==true, open() fails, excluded from
//  resolve()/pathOf(), emitted by enumerate() with tombstone=true.
// ============================================================================

#include "AssetVfs.hpp"

#include <filesystem>

namespace lux::asset
{
    class LUX_RESOURCE_PUBLIC PakAssetProvider final : public IAssetProvider
    {
    public:
        /// Open + fully validate a .luxpak (footer, index hash, bounds, PATH
        /// canonicality). Returns a human-readable reason on failure. The
        /// returned provider keeps the file handle open for entry reads.
        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<PakAssetProvider>, std::string>
        loadFromFile(const std::filesystem::path& pak_path);

        ~PakAssetProvider() override;

        /// The advisory mount root recorded at cook time ("/Game"). The
        /// actual root is whatever the caller passes to AssetVfs::mount.
        [[nodiscard]] const std::string& mountHint() const noexcept;

        [[nodiscard]] const std::filesystem::path& pakPath() const noexcept;

        /// Total ENTB rows (tombstones included).
        [[nodiscard]] std::size_t assetCount() const noexcept;

        // -- IAssetProvider --------------------------------------------------
        [[nodiscard]] std::optional<asset_id_t>
        resolve(std::string_view rel_vpath) const override;

        [[nodiscard]] bool contains(const asset_id_t& id) const override;

        [[nodiscard]] lux::cxx::expected<AssetBlob, EAssetError>
        open(const asset_id_t& id) const override;

        void enumerate(
            const std::function<void(const ProviderEntry&)>& fn) const override;

        [[nodiscard]] std::optional<std::string>
        pathOf(const asset_id_t& id) const override;

    private:
        PakAssetProvider();

        struct Data;                 ///< Holds the decoded index + file stream
        std::unique_ptr<Data> d_;    ///  (pinclude codec types stay private).
    };

} // namespace lux::asset
