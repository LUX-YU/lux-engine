#pragma once
// ============================================================================
//  PakAssetProvider — IAssetProvider over an immutable .luxpak container
//  (the shipped-build mount; the editor uses its Authoring loose provider).
//
//  The paged pak index IS the cooked asset registry. Provider startup retains
//  only the Header and two B+tree root pages; lookup performs independent
//  positional reads through a fixed-capacity page LRU. open() is a seek+read
//  of the verbatim payload plus SHA-256 verification. Tombstone entries honor
//  shadow-delete contract: contains()==true, open() fails, excluded from
//  resolve()/pathOf(), emitted by enumerate() with tombstone=true.
// ============================================================================

#include <lux/engine/resource/asset/storage/AssetProvider.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <filesystem>
#include <cstdint>

namespace lux::asset
{
    struct PakProviderStats final
    {
        std::uint64_t index_page_hits{0u};
        std::uint64_t index_page_misses{0u};
        std::uint64_t index_pages_evicted{0u};
        std::uint64_t index_pages_resident{0u};
        std::uint64_t metadata_resident_bytes{0u};
        std::uint64_t metadata_resident_high_water{0u};
    };

    class LUX_ASSET_PUBLIC PakAssetProvider final : public IAssetProvider
    {
    public:
        /// Open a LUXPAK v2 and validate its Header plus Entry/Path roots.
        /// Descendant pages are verified lazily against their parent digest.
        /// Every page/payload read owns an independent cursor, so unrelated
        /// World Sections can load concurrently.
        [[nodiscard]] static lux::cxx::expected<std::shared_ptr<PakAssetProvider>, std::string>
        loadFromFile(const std::filesystem::path& pak_path);

        ~PakAssetProvider() override;

        /// The advisory mount root recorded at cook time ("/Game"). The
        /// actual root is whatever the caller passes to AssetVfs::mount.
        [[nodiscard]] const std::string& mountHint() const noexcept;

        [[nodiscard]] const std::filesystem::path& pakPath() const noexcept;

        /// Total ENTB rows (tombstones included).
        [[nodiscard]] std::size_t assetCount() const noexcept;

        /// Lightweight fixed-cache diagnostics. This never walks the index.
        [[nodiscard]] PakProviderStats stats() const noexcept;

        // -- IAssetProvider --------------------------------------------------
        [[nodiscard]] std::optional<AssetId> resolve(std::string_view rel_vpath) const override;

        [[nodiscard]] bool contains(const AssetId& id) const override;

        [[nodiscard]] lux::cxx::expected<AssetBlob, EAssetStorageError> open(const AssetId& id) const override;

        void enumerate(const std::function<void(const ProviderEntry&)>& fn) const override;

        [[nodiscard]] std::optional<std::string> pathOf(const AssetId& id) const override;

    private:
        PakAssetProvider();

        struct Data;
        std::unique_ptr<Data> d_;
    };

} // namespace lux::asset
