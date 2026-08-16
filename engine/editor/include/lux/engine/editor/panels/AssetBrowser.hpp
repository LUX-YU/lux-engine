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
#include <lux/engine/resource/asset/Asset.hpp>           // EAssetType, asset_id_t
#include <lux/engine/ui/Panel.hpp>
#include <lux/engine/ui/FuzzySearchIndex.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
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

    /// Main-thread command emitted when the user activates an asset entry.
    /// EditorShell is the single router and selects the matching editor by type.
    struct ActivateAssetCommand
    {
        lux::asset::asset_id_t id;
        lux::asset::EAssetType type;
    };

    /// Command emitted when the user picks "Create Material Instance" from a graph
    /// material's right-click menu. `parent` is the parent MATERIAL's id; the
    /// subscriber authors + persists a new MaterialInstanceAsset.
    struct CreateMaterialInstanceCommand
    {
        lux::asset::asset_id_t parent;
    };

    /// Command emitted by the blank-area right-click "New …" menu: create
    /// a fresh asset of `type` IN the folder the browser is currently showing.
    /// The subscriber (the shell) authors + persists it and opens its editor.
    struct CreateAssetCommand
    {
        lux::asset::EAssetType type;
        std::filesystem::path  folder;   ///< absolute; the browser's cwd at click time
        /// A-6: non-empty for "New Script ▸ C++ ▸ <behavior>" — the registered
        /// behavior name the authored CppBehaviorScript asset references.
        /// Empty = the Lua-template script (the A-3b flow).
        std::string            cpp_behavior;
    };

    /// Command emitted when the user picks "Delete…" from an asset's menu.
    /// AssetDeleteController owns referencer scans, confirmation and deletion.
    struct DeleteAssetCommand
    {
        lux::asset::asset_id_t id;
        lux::asset::EAssetType type;
        std::filesystem::path  abs_path;   ///< 落盘文件(强删时一并移除)
        std::string            name;       ///< display name(对话框用)
    };

    class LUX_EDITOR_PUBLIC AssetBrowser : public lux::ui::Panel
    {
    public:
        using ActivateHandler = lux::cxx::move_only_function<void(
            const ActivateAssetCommand&)>;
        using CreateInstanceHandler = lux::cxx::move_only_function<void(
            const CreateMaterialInstanceCommand&)>;
        using CreateAssetHandler = lux::cxx::move_only_function<void(
            const CreateAssetCommand&)>;
        using DeleteAssetHandler = lux::cxx::move_only_function<void(
            const DeleteAssetCommand&)>;

        /// How the entry area lays out assets. List = compact rows (glyphs);
        /// Grid = thumbnail tiles. List is the default.
        enum class EViewMode { LIST, GRID };

        AssetBrowser(std::string title, std::shared_ptr<AssetManager> manager);
        ~AssetBrowser() override;

        // ── Working-set management ───────────────────────────────────────

        /// Set the project's Content/ root (or any folder). Triggers a
        /// rescan + resets folder navigation to that root. Empty path = clear.
        void setWorkingDirectory(std::filesystem::path path);

        /// Force a rescan now. Cheap (filesystem walk only); call after
        /// any import finishes so newly-written files show up.
        void rescan();

        /// Wire the (editor-owned) thumbnail service. When set, each asset
        /// entry requests a thumbnail and draws it in place of the type glyph
        /// once ready. Null = glyphs only (the default).
        void setThumbnailService(lux::editor::ThumbnailService* svc) noexcept
        { thumbnail_service_ = svc; }

        /// Per-asset describe hook — extra tooltip lines under
        /// the name/path for a hovered asset (type-specific: script kind +
        /// module, instance parent, mesh counts …). CONTRACT: must NOT trigger
        /// loads (hover is a hot path) — describe only what is already in
        /// memory, or say so. Null = name + virtual path only (the default).
        void setDescribeHook(std::function<std::vector<std::string>(
                                 const lux::asset::asset_id_t&,
                                 lux::asset::EAssetType)> fn) noexcept
        { describe_ = std::move(fn); }

        /// A-6 C++ manifest: provider for the registered C++ behavior names —
        /// the "New Script ▸ C++" submenu lists them; picking one invokes the
        /// create command with `cpp_behavior` set (the shell authors the
        /// CppBehaviorScript asset). Null = no C++ submenu.
        void setCppScriptNamesProvider(
            std::function<std::vector<std::string_view>()> p) noexcept
        { cpp_script_names_ = std::move(p); }

        void setActivateHandler(ActivateHandler handler) noexcept
        { activate_handler_ = std::move(handler); }

        void setCreateInstanceHandler(CreateInstanceHandler handler) noexcept
        { create_instance_handler_ = std::move(handler); }

        void setCreateAssetHandler(CreateAssetHandler handler) noexcept
        { create_asset_handler_ = std::move(handler); }

        void setDeleteAssetHandler(DeleteAssetHandler handler) noexcept
        { delete_asset_handler_ = std::move(handler); }

    protected:
        void beforePaint() override;
        void paint() override;
        void afterPaint() override;

    private:
        ActivateHandler       activate_handler_;
        CreateInstanceHandler create_instance_handler_;
        CreateAssetHandler    create_asset_handler_;
        DeleteAssetHandler    delete_asset_handler_;

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

    public:
        // ── 驻留失败标记(T14 失败可见性,裁决J4):终态失败的资产在浏览
        //    器里标红 + 悬浮显示原因;内容修复/重注册时装配层清除。────────
        void markResidencyFailed(const lux::asset::asset_id_t& id,
                                 std::string                   reason)
        {
            residency_failed_[id] = std::move(reason);
        }
        void clearResidencyFailed(const lux::asset::asset_id_t& id)
        {
            residency_failed_.erase(id);
        }

    private:

        /// Tiled view with rendered thumbnails (via `thumbnail_service_`), with a
        /// procedural type glyph as the placeholder until a thumbnail is ready.
        void paintEntryGrid();

        // -----------------------------------------------------------------
        //  State
        // -----------------------------------------------------------------

        std::shared_ptr<AssetManager>     asset_mgr_;
        lux::editor::ThumbnailService*    thumbnail_service_{nullptr}; ///< not owned
        std::function<std::vector<std::string>(
            const lux::asset::asset_id_t&, lux::asset::EAssetType)>
                                          describe_;   ///< A-4 tooltip enricher (may be null)
        std::function<std::vector<std::string_view>()>
                                          cpp_script_names_;   ///< A-6 New Script ▸ C++ manifest
        std::filesystem::path             root_;   ///< Content/ root (or any folder)
        std::filesystem::path             cwd_;    ///< currently-displayed folder
        std::vector<AssetBrowserEntry>    entries_;
        // Fuzzy search over the current folder's entries (rebuilt on each scan).
        lux::ui::FuzzySearchIndex         search_index_;
        std::string                       search_query_;
        std::vector<std::uint32_t>        filtered_;   ///< entry indices passing the search
        // Fuzzy-filter cache (C8): re-query only when the query text or the
        // folder contents (scan_revision_) changed — not every frame.
        std::string                       last_query_;
        std::uint64_t                     scan_revision_{0};
        std::uint64_t                     last_scan_rev_{static_cast<std::uint64_t>(-1)};
        // Folder-tree listings cache (C8): per-directory subfolder lists,
        // invalidated on every rescan — no per-frame disk iteration.
        std::unordered_map<std::string, std::vector<std::filesystem::path>> tree_cache_;
        std::filesystem::path             selection_;  ///< empty when nothing selected
        /// 驻留终态失败的资产 → 原因(标红 + 悬浮;见 markResidencyFailed)。
        std::unordered_map<lux::asset::asset_id_t, std::string> residency_failed_;
        // The content browser is primarily visual: start in GRID so visible
        // assets actually request their asynchronous thumbnails. LIST remains
        // available from the toolbar for dense metadata browsing.
        EViewMode                         view_mode_{EViewMode::GRID};
        bool                              show_folder_tree_{false}; ///< left tree (folded by default; toolbar toggle)

        // Throttle the FS walk to once-per-frame at most. paint() is invoked
        // a few times per frame on docking-mode rebuilds; rescanning the
        // disk each call would be wasteful on large content roots.
        std::chrono::steady_clock::time_point last_scan_{};
    };
} // namespace lux::editor
