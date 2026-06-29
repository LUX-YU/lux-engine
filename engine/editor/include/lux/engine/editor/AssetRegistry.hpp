#pragma once
// ============================================================================
//  AssetRegistry — a lightweight, project-wide index of EVERY recognized asset's
//  metadata, built by a HEADER-ONLY scan (no payload load). The editor's
//  "find almost any resource" foundation (UE FAssetData-style): pickers filter by
//  type + fuzzy-search names, and load the full asset only on selection.
// ============================================================================

#include <lux/engine/editor/visibility.h>
#include <lux/engine/asset/Asset.hpp>            // EAssetType, asset_id_t
#include <lux/engine/asset/LooseDirProvider.hpp> // the SSOT this is a view of

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace lux::editor
{
    /// One indexed asset (UE FAssetData-equivalent): just the cheap header
    /// metadata, never the payload.
    struct AssetMeta
    {
        lux::asset::asset_id_t id{};
        lux::asset::EAssetType type{ lux::asset::EAssetType::UNKNOWN };
        std::string            name;       ///< file stem (what the user sees)
        std::string            rel_path;   ///< path relative to the content root
    };

    class LUX_EDITOR_PUBLIC AssetRegistry
    {
    public:
        /// Recursively scan a content root, reading each .luxasset/.luxmodel
        /// header (magic + UUID) — cheap even for thousands of assets. Replaces
        /// the current index. Since VP-P4 this delegates to a LooseDirProvider
        /// (the registry is a VIEW over the same provider the editor mounts at
        /// /Game — one path->id table, not two).
        void scan(const std::filesystem::path& content_root);

        /// The provider backing this registry — what LuxEditor mounts into the
        /// AssetVfs. Stable across refresh(); replaced only when scan() gets a
        /// different root. Null before the first scan.
        [[nodiscard]] const std::shared_ptr<lux::asset::LooseDirProvider>&
        provider() const noexcept
        {
            return provider_;
        }

        /// Rescan the last-scanned root (manual freshness until a file-watcher).
        void refresh() { scan(root_); }

        /// The content root last scanned (where new editor-authored assets are
        /// written). Empty until the first scan().
        const std::filesystem::path& root() const noexcept { return root_; }

        const std::vector<AssetMeta>& all() const noexcept { return assets_; }
        std::size_t                   size() const noexcept { return assets_.size(); }

        /// Indices into all() whose type matches (for a typed picker).
        std::vector<std::uint32_t> ofType(lux::asset::EAssetType type) const;

        /// Metadata for a given asset id, or nullptr if not indexed. Linear scan
        /// (the index is small and this is only hit when a picker/inspector needs
        /// a name); add a hash map if it ever shows up in a profile.
        const AssetMeta* find(const lux::asset::asset_id_t& id) const noexcept
        {
            for (const auto& m : assets_)
                if (m.id == id) return &m;
            return nullptr;
        }

    private:
        std::filesystem::path  root_;
        std::vector<AssetMeta> assets_;
        std::shared_ptr<lux::asset::LooseDirProvider> provider_;
    };

} // namespace lux::editor
