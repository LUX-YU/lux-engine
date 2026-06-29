#include <lux/engine/editor/panels/AssetBrowser.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/AssetSerDeser.hpp>   // asset_magic_number_of
#include <lux/engine/asset/AssetVfs.hpp>        // vfs->pathOf for tooltips
#include <lux/engine/ui/AssetDragDrop.hpp>
#include <lux/engine/editor/panels/AssetTypeRegistry.hpp>   // type chip/glyph metadata
#include <lux/engine/editor/thumbnail/ThumbnailService.hpp> // GRID view requestThumbnail
#include <lux/engine/asset/AssetHeaderProbe.hpp>            // shared readAssetHeader/assetTypeOfMagic

#include <imgui.h>
#include <imgui_stdlib.h>   // ImGui::InputTextWithHint(std::string)

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace lux::editor
{
    // AssetDragPayload + its tag are the generic drag-drop contract — they stay
    // in lux::ui (a shared payload type); pull them in here by name.
    using lux::ui::AssetDragPayload;
    using lux::ui::kAssetDragPayloadTag;

    namespace
    {
        // Header probe (readAssetHeader) + magic->type (assetTypeOfMagic) live
        // in the asset module's <lux/engine/asset/AssetHeaderProbe.hpp> — the
        // SSOT shared with the AssetRegistry, import pipeline and pak cooker.

        // Type label + chip colour + glyph now come from
        // lux::editor::assetTypeDesc() (AssetTypeRegistry.hpp), so a new
        // EAssetType only needs a row there — not edits here.

        // Compact human-readable file size — "12.3 KB" style.
        std::string formatSize(std::uint64_t b)
        {
            if (b >= 1024ull * 1024 * 1024)
                return std::format("{:.2f} GB", static_cast<double>(b) / (1024.0 * 1024.0 * 1024.0));
            if (b >= 1024ull * 1024)
                return std::format("{:.2f} MB", static_cast<double>(b) / (1024.0 * 1024.0));
            if (b >= 1024)
                return std::format("{:.2f} KB", static_cast<double>(b) / 1024.0);
            return std::format("{} B", b);
        }
    } // namespace

    // -------------------------------------------------------------------------

    AssetBrowser::AssetBrowser(std::string title, std::shared_ptr<AssetManager> manager)
        : Panel(std::move(title))
        , asset_mgr_(std::move(manager))
    {
    }

    AssetBrowser::~AssetBrowser() = default;

    // -------------------------------------------------------------------------

    void AssetBrowser::setWorkingDirectory(std::filesystem::path path)
    {
        std::error_code ec;
        if (!path.empty() && std::filesystem::exists(path, ec) && !ec)
        {
            root_ = std::move(path);
            cwd_  = root_;
        }
        else
        {
            root_.clear();
            cwd_.clear();
        }
        selection_.clear();
        entries_.clear();
        if (!cwd_.empty()) scanCwd();
    }

    void AssetBrowser::rescan()
    {
        if (!cwd_.empty()) scanCwd();
    }

    // -------------------------------------------------------------------------

    void AssetBrowser::scanCwd()
    {
        entries_.clear();
        last_scan_ = std::chrono::steady_clock::now();

        std::error_code ec;
        if (cwd_.empty() || !std::filesystem::is_directory(cwd_, ec) || ec)
            return;

        for (const auto& de : std::filesystem::directory_iterator(cwd_, ec))
        {
            AssetBrowserEntry e;
            e.abs_path     = de.path();
            e.display_name = de.path().stem().string();
            e.is_directory = de.is_directory(ec);
            if (e.is_directory)
            {
                entries_.push_back(std::move(e));
                continue;
            }

            // Only `.luxasset` and `.luxmodel` are recognized for the
            // chip + type column. Foreign files (e.g. dropped source PNGs
            // that haven't been imported yet) still appear in the list
            // but render as UNKNOWN — that hints the user to Import them.
            const auto ext = de.path().extension().string();
            e.is_model = (ext == ".luxmodel");
            const bool is_known_ext = (ext == ".luxasset") || e.is_model;
            if (is_known_ext)
            {
                const auto probe = lux::asset::readAssetHeader(de.path());
                e.asset_type = lux::asset::assetTypeOfMagic(probe.magic);
                e.asset_id   = probe.id;
            }
            e.size_bytes = de.file_size(ec);
            entries_.push_back(std::move(e));
        }

        std::sort(entries_.begin(), entries_.end(),
            [](const AssetBrowserEntry& a, const AssetBrowserEntry& b)
            {
                // Directories first; then files; both alphabetical
                // (case-insensitive on Windows feel via lowercase compare).
                if (a.is_directory != b.is_directory) return a.is_directory;
                auto lower = [](std::string s)
                {
                    for (char& c : s)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return s;
                };
                return lower(a.display_name) < lower(b.display_name);
            });

        // Rebuild the fuzzy-search corpus for the new entry set. All preprocessing
        // (lowercased char-mask + interned name) is amortized here so the
        // per-keystroke query() stays cheap. See FuzzySearchIndex.
        std::vector<std::string> names;
        names.reserve(entries_.size());
        for (const auto& e : entries_) names.push_back(e.display_name);
        search_index_.build(names);
    }

    // -------------------------------------------------------------------------

    void AssetBrowser::beforePaint() {}
    void AssetBrowser::afterPaint() {}

    void AssetBrowser::paint()
    {
        if (root_.empty())
        {
            ImGui::TextDisabled("No project open — open a project to browse Content/.");
            return;
        }

        // The folder tree (left) duplicates the breadcrumb + double-click-to-
        // enter navigation, so it is FOLDED by default to give the entry grid
        // the full width; the "Tree" toolbar button toggles it back on for
        // cross-folder jumps. When shown, a persistent 2-column splitter.
        if (show_folder_tree_)
        {
            ImGui::Columns(2, "##asset_browser_split", true);
            if (ImGui::GetColumnWidth(0) < 32.0f)
                ImGui::SetColumnWidth(0, 200.0f);
            paintFolderTree(root_);
            ImGui::NextColumn();
            paintEntryArea();
            ImGui::Columns(1);
        }
        else
        {
            paintEntryArea();
        }
    }

    void AssetBrowser::paintFolderTree(const std::filesystem::path& dir)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return;

        // Render the dir itself as a TreeNode. The Content/ root is open
        // by default; deeper folders are collapsed.
        const bool is_root = (dir == root_);
        const std::string label = is_root
            ? "Content"
            : dir.filename().string();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_OpenOnDoubleClick
                                 | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (is_root) flags |= ImGuiTreeNodeFlags_DefaultOpen;
        if (dir == cwd_) flags |= ImGuiTreeNodeFlags_Selected;

        const bool open = ImGui::TreeNodeEx(label.c_str(), flags);

        // Clicking the node label navigates the right pane to this folder.
        // We test IsItemClicked() AFTER TreeNodeEx so the click is on the
        // node we just submitted, regardless of expand state.
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            cwd_ = dir;
            scanCwd();
        }

        if (open)
        {
            std::vector<std::filesystem::path> subdirs;
            for (const auto& de : std::filesystem::directory_iterator(dir, ec))
            {
                if (de.is_directory(ec)) subdirs.push_back(de.path());
            }
            std::sort(subdirs.begin(), subdirs.end());
            for (const auto& sub : subdirs)
                paintFolderTree(sub);
            ImGui::TreePop();
        }
    }

    void AssetBrowser::paintEntryArea()
    {
        // ── Breadcrumb row (Content > Models > CesiumMan) ─────────────────
        //
        // Each segment is a button — clicking it warps `cwd_` up the path.
        // We build the chain from cwd_ back to root_ then iterate forward.
        std::vector<std::filesystem::path> chain;
        {
            std::filesystem::path p = cwd_;
            while (!p.empty() && p != root_.parent_path())
            {
                chain.push_back(p);
                if (p == root_) break;
                p = p.parent_path();
            }
            std::reverse(chain.begin(), chain.end());
        }
        for (std::size_t i = 0; i < chain.size(); ++i)
        {
            const bool is_root = (chain[i] == root_);
            const std::string label = is_root ? "Content"
                                              : chain[i].filename().string();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton(label.c_str()))
            {
                cwd_ = chain[i];
                scanCwd();
            }
            ImGui::PopID();
            if (i + 1 < chain.size())
            {
                ImGui::SameLine();
                ImGui::TextUnformatted(">");
                ImGui::SameLine();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("   %zu items", entries_.size());

        // Manual refresh — handy when something dropped files into Content/
        // outside the editor's import path.
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh##asset_browser"))
            scanCwd();

        // View-mode toggle — the button shows the mode it switches TO. Grid mode
        // is where rendered thumbnails appear (generated asynchronously, so the
        // UI keeps responding while they pop in).
        ImGui::SameLine();
        if (ImGui::SmallButton(view_mode_ == EViewMode::LIST ? "Grid##view" : "List##view"))
            view_mode_ = (view_mode_ == EViewMode::LIST) ? EViewMode::GRID : EViewMode::LIST;

        // Folder-tree toggle — the tree is folded by default (it duplicates the
        // breadcrumb); show it for cross-folder jumps.
        ImGui::SameLine();
        if (ImGui::SmallButton(show_folder_tree_ ? "Hide Tree##tree" : "Tree##tree"))
            show_folder_tree_ = !show_folder_tree_;

        // ── Fuzzy search box ──────────────────────────────────────────────
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##assetsearch", "search (fuzzy)...", &search_query_);

        // Filter the current folder by fuzzy match (empty query -> all entries,
        // in scan order). Survivors come back sorted by match score.
        filtered_.clear();
        filtered_.reserve(entries_.size());
        for (const auto& m : search_index_.query(search_query_))
            filtered_.push_back(m.index);

        ImGui::Separator();

        if (view_mode_ == EViewMode::GRID) paintEntryGrid();
        else                               paintEntryList();
    }

    void AssetBrowser::paintEntryList()
    {
        // ── Entry list ────────────────────────────────────────────────────
        //
        // Each row: [type chip] [name] [size, right-aligned]
        // Double-click on a directory enters it. Single-click selects.
        for (std::uint32_t fi_ : filtered_)
        {
            const AssetBrowserEntry& e = entries_[fi_];
            ImGui::PushID(e.abs_path.string().c_str());

            // Type icon: a colour-filled chip (quick class id) with a
            // code-drawn glyph on top (shape id), both from the
            // AssetTypeRegistry. The 3-letter tag now lives in a hover
            // tooltip. A square reserves the column so rows stay aligned
            // regardless of name length.
            const float icon_sz = ImGui::GetTextLineHeight();
            const ImU32 chip_color = e.is_directory
                ? lux::editor::kFolderChipColor
                : lux::editor::assetTypeDesc(e.asset_type).chip_color;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 cp = ImGui::GetCursorScreenPos();
            const ImVec2 tl = cp;
            const ImVec2 br = ImVec2(cp.x + icon_sz, cp.y + icon_sz);
            constexpr ImU32 kGlyph = IM_COL32(28, 28, 28, 235);

            // LIST mode: always the procedural type glyph. Rendered thumbnails
            // are intentionally NOT requested here — at list-row size they're too
            // small to be worth the (currently blocking) generation cost, which
            // also stutters the editor. Thumbnails are deferred to the future
            // GRID view mode, which will call thumbnail_service_->requestThumbnail.
            dl->AddRectFilled(tl, br, chip_color, 3.0f);
            if (e.is_directory)
                lux::editor::drawFolderGlyph(dl, tl, br, kGlyph);
            else
                lux::editor::assetTypeDesc(e.asset_type).draw_glyph(dl, tl, br, kGlyph);

            ImGui::Dummy(ImVec2(icon_sz + 6.0f, icon_sz));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", e.is_directory
                    ? "Folder"
                    : lux::editor::assetTypeDesc(e.asset_type).name);
            ImGui::SameLine();

            const bool selected = (selection_ == e.abs_path);
            if (ImGui::Selectable(
                    e.display_name.c_str(), selected,
                    ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2(0, ImGui::GetTextLineHeight())))
            {
                selection_ = e.abs_path;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    if (e.is_directory)
                    {
                        cwd_ = e.abs_path;
                        selection_.clear();
                        scanCwd();
                    }
                    else if (e.asset_type != lux::asset::EAssetType::UNKNOWN)
                    {
                        activated.emit({e.asset_id, e.asset_type});
                    }
                }
            }
            // Right-click a baked graph material -> author a Material Instance of it.
            if (!e.is_directory
                && e.asset_type == lux::asset::EAssetType::MATERIAL
                && ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Create Material Instance"))
                    create_instance_requested.emit({e.asset_id});
                ImGui::EndPopup();
            }

            // Drag-source: only well-formed assets carry a usable UUID
            // payload. Directories and foreign files just don't bind a
            // source — drag-from-empty is silently dropped by ImGui.
            if (!e.is_directory &&
                e.asset_type != lux::asset::EAssetType::UNKNOWN &&
                ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                AssetDragPayload payload{};
                std::memcpy(payload.uuid_bytes,
                            e.asset_id.as_bytes().data(),
                            sizeof(payload.uuid_bytes));
                payload.asset_type =
                    static_cast<std::uint8_t>(e.asset_type);
                payload.is_model = e.is_model ? 1 : 0;
                // Copy the display name, leaving room for the NUL.
                const std::size_t name_n =
                    std::min(sizeof(payload.display_name) - 1,
                             e.display_name.size());
                std::memcpy(payload.display_name,
                            e.display_name.data(), name_n);

                ImGui::SetDragDropPayload(
                    kAssetDragPayloadTag, &payload, sizeof(payload),
                    ImGuiCond_Once);

                // Drag-ghost shown under the cursor while held — keep it
                // simple: chip + name; visual continuity with the grid row.
                ImGui::Text("%s  %s",
                            lux::editor::assetTypeDesc(e.asset_type).label,
                            e.display_name.c_str());
                ImGui::EndDragDropSource();
            }

            // Size column — drawn on the same row right-aligned. We do this
            // via SameLine + SetCursorPosX so it follows panel resizes.
            if (!e.is_directory)
            {
                const std::string sz = formatSize(e.size_bytes);
                const float avail = ImGui::GetContentRegionAvail().x;
                const float text_w = ImGui::CalcTextSize(sz.c_str()).x;
                if (text_w + 8.0f < avail)
                {
                    ImGui::SameLine(0, 0);
                    ImGui::SetCursorPosX(
                        ImGui::GetCursorPosX() + avail - text_w - 4.0f);
                    ImGui::TextDisabled("%s", sz.c_str());
                }
            }

            ImGui::PopID();
        }
    }

    void AssetBrowser::paintEntryGrid()
    {
        // Tile metrics. Each cell = an icon square + one truncated label line.
        const float tile    = 96.0f;
        const float spacing = 10.0f;
        const float label_h = ImGui::GetTextLineHeight();
        const float cell_h  = tile + label_h + 6.0f;
        const float cell_w  = tile + spacing;

        const float avail = ImGui::GetContentRegionAvail().x;
        const int   cols  = std::max(1, static_cast<int>((avail + spacing) / cell_w));

        constexpr ImU32 kGlyph = IM_COL32(28, 28, 28, 235);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        int col = 0;
        for (std::uint32_t fi_ : filtered_)
        {
            const AssetBrowserEntry& e = entries_[fi_];
            ImGui::PushID(e.abs_path.string().c_str());

            const ImVec2 cp       = ImGui::GetCursorScreenPos();
            const bool   selected = (selection_ == e.abs_path);

            // One interactive widget covering the whole cell (icon + label); the
            // visuals are drawn on top via the draw list.
            if (ImGui::Selectable("##tile", selected,
                                  ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(tile, cell_h)))
            {
                selection_ = e.abs_path;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    if (e.is_directory)
                    {
                        cwd_ = e.abs_path;
                        selection_.clear();
                        scanCwd();
                    }
                    else if (e.asset_type != lux::asset::EAssetType::UNKNOWN)
                    {
                        activated.emit({e.asset_id, e.asset_type});
                    }
                }
            }
            // Right-click a baked graph material -> author a Material Instance of it.
            if (!e.is_directory
                && e.asset_type == lux::asset::EAssetType::MATERIAL
                && ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Create Material Instance"))
                    create_instance_requested.emit({e.asset_id});
                ImGui::EndPopup();
            }

            // Drag-source — binds to the Selectable above (assets only).
            if (!e.is_directory &&
                e.asset_type != lux::asset::EAssetType::UNKNOWN &&
                ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
            {
                AssetDragPayload payload{};
                std::memcpy(payload.uuid_bytes, e.asset_id.as_bytes().data(),
                            sizeof(payload.uuid_bytes));
                payload.asset_type = static_cast<std::uint8_t>(e.asset_type);
                payload.is_model   = e.is_model ? 1 : 0;
                const std::size_t name_n =
                    std::min(sizeof(payload.display_name) - 1, e.display_name.size());
                std::memcpy(payload.display_name, e.display_name.data(), name_n);
                ImGui::SetDragDropPayload(kAssetDragPayloadTag, &payload, sizeof(payload),
                                          ImGuiCond_Once);
                ImGui::Text("%s  %s", lux::editor::assetTypeDesc(e.asset_type).label,
                            e.display_name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered())
            {
                // Name + the asset's virtual path (resolved through the VFS so
                // every mount answers, not just loose /Game files).
                std::string tip = e.display_name;
                if (!e.is_directory && asset_mgr_)
                    if (const auto vfs = asset_mgr_->vfs())
                        if (auto p = vfs->pathOf(e.asset_id))
                        {
                            tip += "\n";
                            tip += *p;
                        }
                ImGui::SetTooltip("%s", tip.c_str());
            }

            // Icon square background.
            const ImVec2 itl = cp;
            const ImVec2 ibr = ImVec2(cp.x + tile, cp.y + tile);
            dl->AddRectFilled(itl, ibr, IM_COL32(36, 36, 42, 255), 4.0f);

            // Thumbnail (async) or a procedural type-glyph placeholder.
            ImTextureID thumb = 0;
            if (thumbnail_service_ && !e.is_directory &&
                e.asset_type != lux::asset::EAssetType::UNKNOWN)
                thumb = thumbnail_service_->requestThumbnail(e.asset_id, e.asset_type);

            if (thumb)
            {
                dl->AddImage(thumb, ImVec2(itl.x + 2, itl.y + 2), ImVec2(ibr.x - 2, ibr.y - 2));
            }
            else
            {
                const ImU32 chip = e.is_directory
                    ? lux::editor::kFolderChipColor
                    : lux::editor::assetTypeDesc(e.asset_type).chip_color;
                const float  g = tile * 0.5f;
                const ImVec2 gtl(cp.x + (tile - g) * 0.5f, cp.y + (tile - g) * 0.5f);
                const ImVec2 gbr(gtl.x + g, gtl.y + g);
                dl->AddRectFilled(gtl, gbr, chip, 4.0f);
                if (e.is_directory)
                    lux::editor::drawFolderGlyph(dl, gtl, gbr, kGlyph);
                else
                    lux::editor::assetTypeDesc(e.asset_type).draw_glyph(dl, gtl, gbr, kGlyph);
            }

            // Selection outline (drawn over the icon so it stays visible).
            if (selected)
                dl->AddRect(cp, ImVec2(cp.x + tile, cp.y + cell_h),
                            IM_COL32(255, 200, 80, 255), 4.0f, 0, 2.0f);

            // Label — truncated to the tile width, centered under the icon.
            {
                std::string s = e.display_name;
                bool truncated = false;
                while (s.size() > 1 && ImGui::CalcTextSize(s.c_str()).x > tile - 4.0f)
                {
                    s.pop_back();
                    truncated = true;
                }
                if (truncated && s.size() > 2)
                    s = s.substr(0, s.size() - 2) + "..";
                const float tw = ImGui::CalcTextSize(s.c_str()).x;
                dl->AddText(ImVec2(cp.x + (tile - tw) * 0.5f, cp.y + tile + 3.0f),
                            IM_COL32(220, 220, 220, 255), s.c_str());
            }

            ImGui::PopID();

            if (++col >= cols) col = 0;
            else               ImGui::SameLine(0.0f, spacing);
        }
    }
} // namespace lux::editor
