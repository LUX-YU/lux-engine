#pragma once
/**
 * @file AssetBrowser.hpp
 * @brief Read-only browser for a project's Content/ folder.
 *
 * Stage A (this version): scans the project's Content/ tree once on
 * `setWorkingDirectory`, presents a two-pane layout (folder tree + entry
 * grid), and lets the user navigate. Each entry shows a type chip + name +
 * file size. **No drag-drop yet** — Stages B / C plug into the same model.
 *
 * Scanning + on-disk asset registration are decoupled: this class only
 * walks the filesystem and parses each file's `AssetFileHeader` magic to
 * recognize asset type. The editor (LuxEditor::openProject) is the one
 * that actually calls each `*SerDeser::fromLuxAsset` to register the
 * assets into the live AssetManager — that way the browser still works on
 * partial / corrupt project trees without dragging the heavy load path
 * into the panel.
 */

#include <lux/engine/editor/visibility.h>
#include <lux/engine/asset/Asset.hpp>           // EAssetType, asset_id_t
#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/FuzzySearchIndex.hpp>

#include <lux/cxx/event/Signal.hpp>             // activated / create_instance_requested

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lux::asset  { class AssetManager; }
namespace lux::editor { class ThumbnailService; }

namespace lux::editor
{
    using AssetManager = lux::asset::AssetManager;

    /// One row in the browser grid — a directory or a recognized asset file.
    /// Directories sort to the top alphabetically; files follow.
    struct AssetBrowserEntry
    {
        std::filesystem::path  abs_path;
        std::string            display_name;   ///< filename stem (no ext)
        bool                   is_directory{false};

        // Valid only when !is_directory and the file's magic could be
        // decoded. `asset_type` falls back to `EAssetType::UNKNOWN` for
        // foreign / unrecognized files.
        lux::asset::EAssetType asset_type{lux::asset::EAssetType::UNKNOWN};
        bool                   is_model{false}; ///< true for `.luxmodel`
        std::uint64_t          size_bytes{0};

        /// UUID from the file's `AssetFileHeader::info.id`. All-zero when
        /// the file could not be parsed. Carried in the DragDrop payload
        /// so the viewport-side spawn path can dereference the asset
        /// without re-reading the file.
        lux::asset::asset_id_t asset_id{};
    };

    /// Emitted when the user double-clicks an ASSET entry (not a directory).
    /// Subscribers open the asset in its editor (e.g. a material -> the material
    /// graph panel). A signal (not a single callback) so multiple panels can react.
    struct AssetActivated
    {
        lux::asset::asset_id_t id;
        lux::asset::EAssetType type;
    };

    /// Emitted when the user picks "Create Material Instance" from a graph
    /// material's right-click menu. `parent` is the parent MATERIAL's id; the
    /// subscriber authors + persists a new MaterialInstanceAsset.
    struct CreateInstanceRequested
    {
        lux::asset::asset_id_t parent;
    };

    class LUX_EDITOR_PUBLIC AssetBrowser : public lux::ui::Panel
    {
    public:
        /// How the entry area lays out assets. List = compact rows (glyphs);
        /// Grid = thumbnail tiles. List is the default.
        enum class EViewMode { LIST, GRID };

        AssetBrowser(std::string title, std::shared_ptr<AssetManager> manager);
        ~AssetBrowser() override;

        // ── Working-set management ───────────────────────────────────────

        /// Set the project's Content/ root (or any folder). Triggers a
        /// rescan + drops navigation to that root. Empty path = clear.
        void setWorkingDirectory(std::filesystem::path path);

        [[nodiscard]] const std::filesystem::path& workingDirectory() const noexcept
        { return root_; }

        /// Currently-displayed folder (somewhere at-or-under `workingDirectory()`).
        [[nodiscard]] const std::filesystem::path& currentDirectory() const noexcept
        { return cwd_; }

        /// Force a rescan now. Cheap (filesystem walk only); call after
        /// any import finishes so newly-written files show up.
        void rescan();

        /// Wire the (editor-owned) thumbnail service. When set, each asset
        /// entry requests a thumbnail and draws it in place of the type glyph
        /// once ready. Null = glyphs only (the default).
        void setThumbnailService(lux::editor::ThumbnailService* svc) noexcept
        { thumbnail_service_ = svc; }

        /// Fired on double-clicking an asset entry / picking "Create Material
        /// Instance". Public signals (not single setters): any number of panels
        /// may `connect` — emitting with no subscribers is a harmless no-op. The
        /// host owns the connections (their lifetime must not outlive this panel).
        lux::cxx::event::Signal<AssetActivated>          activated;
        lux::cxx::event::Signal<CreateInstanceRequested> create_instance_requested;

    protected:
        void beforePaint() override;
        void paint() override;
        void afterPaint() override;

    private:
        // -----------------------------------------------------------------
        //  Helpers
        // -----------------------------------------------------------------

        /// Refresh `entries_` for the current `cwd_`. Sorts directories
        /// first, then files alphabetically. Reads `AssetFileHeader` magic
        /// of each `.luxasset` / `.luxmodel` to fill `asset_type`.
        void scanCwd();

        /// Render the left-side folder tree (recursive, lazy expand).
        void paintFolderTree(const std::filesystem::path& dir);

        /// Render the right-side breadcrumb + toolbar, then dispatch to the
        /// active view mode below.
        void paintEntryArea();

        /// Compact one-row-per-entry view (type glyph + name + size). Glyph-only:
        /// thumbnails are too small to be worth generating at list-row size.
        void paintEntryList();

        /// Tiled view with rendered thumbnails (via `thumbnail_service_`), with a
        /// procedural type glyph as the placeholder until a thumbnail is ready.
        void paintEntryGrid();

        // -----------------------------------------------------------------
        //  State
        // -----------------------------------------------------------------

        std::shared_ptr<AssetManager>     asset_mgr_;
        lux::editor::ThumbnailService*    thumbnail_service_{nullptr}; ///< not owned
        std::filesystem::path             root_;   ///< Content/ root (or any folder)
        std::filesystem::path             cwd_;    ///< currently-displayed folder
        std::vector<AssetBrowserEntry>    entries_;
        // Fuzzy search over the current folder's entries (rebuilt on each scan).
        lux::ui::FuzzySearchIndex         search_index_;
        std::string                       search_query_;
        std::vector<std::uint32_t>        filtered_;   ///< entry indices passing the search
        std::filesystem::path             selection_;  ///< empty when nothing selected
        EViewMode                         view_mode_{EViewMode::LIST};
        bool                              show_folder_tree_{false}; ///< left tree (folded by default; toolbar toggle)

        // Throttle the FS walk to once-per-frame at most. paint() is invoked
        // a few times per frame on docking-mode rebuilds; rescanning the
        // disk each call would be wasteful on large content roots.
        std::chrono::steady_clock::time_point last_scan_{};
    };
} // namespace lux::editor
