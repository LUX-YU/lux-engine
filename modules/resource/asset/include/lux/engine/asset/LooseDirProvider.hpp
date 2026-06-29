#pragma once
// ============================================================================
//  LooseDirProvider — IAssetProvider over a loose .luxasset content folder
//  (the editor / development mount; shipped builds use PakAssetProvider).
//
//  rescan() walks the directory recursively (same probe as the editor's
//  AssetRegistry scan: header magic + uuid, never trusting the extension),
//  building path->id and id->file tables. open() reads the file AT CALL
//  TIME — only the tables are cached, never the bytes — so an edit after
//  the scan is visible immediately; only ADD/REMOVE/RENAME needs a rescan
//  (the documented index-staleness window, design §10.3).
// ============================================================================

#include "AssetVfs.hpp"

#include <filesystem>
#include <unordered_map>

namespace lux::asset
{
    class LUX_RESOURCE_PUBLIC LooseDirProvider final : public IAssetProvider
    {
    public:
        explicit LooseDirProvider(std::filesystem::path root_dir);

        /// Rebuild the index from disk. Returns the number of indexed assets.
        /// Collisions are resolved DETERMINISTICALLY (byte-smallest relative
        /// file path wins) and recorded in diagnostics():
        ///  * same vpath twice (same stem, different extension / duplicate)
        ///  * same uuid in two files
        ///  * case-insensitive vpath clash (both stay resolvable — resolve
        ///    is byte-sensitive — but cook will reject; surfaced here so the
        ///    editor can warn at author time)
        ///  * file whose stem/segments violate the VirtualPath charset
        std::size_t rescan();

        [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept
        {
            return diagnostics_;
        }

        [[nodiscard]] const std::filesystem::path& rootDir() const noexcept
        {
            return root_dir_;
        }

        /// Editor convenience: the on-disk file backing an id (absolute).
        [[nodiscard]] std::optional<std::filesystem::path>
        filePathOf(const asset_id_t& id) const;

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
        struct Entry
        {
            asset_id_t            id{};
            EAssetType            type{ EAssetType::UNKNOWN };
            std::string           vpath;   ///< Mount-relative canonical.
            std::filesystem::path file;    ///< Absolute on-disk path.
        };

        std::filesystem::path root_dir_;
        std::vector<Entry>    entries_;
        /// Byte-sensitive (frozen comparison semantics, design §2/§11).
        std::unordered_map<std::string, std::size_t> by_path_;
        std::unordered_map<asset_id_t, std::size_t>  by_id_;
        std::vector<std::string> diagnostics_;
    };

} // namespace lux::asset
