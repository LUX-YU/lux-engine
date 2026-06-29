#pragma once
#include <lux/engine/function/visibility_ui.h>
#include <lux/engine/ui/UIElement.hpp>
#include <lux/engine/ui/FuzzySearchIndex.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lux::ui
{
    /**
     * @brief A modal fuzzy-search PICKER popup (reusable).
     *
     * open() it (e.g. from a button), and on the next draw() it shows a centered
     * popup: a search box + a relevance-sorted, virtualised result list. Selecting
     * an item fires the callback with that item's index (into the last setItems()
     * list) and closes. Reusable for a texture picker, a command palette, a
     * reflection-method picker, ... — backed by FuzzySearchIndex.
     *
     * IMPORTANT: draw() must run in a frame-CLOSED, NON-canvas context (a normal
     * panel paint(), NOT between an imgui-node-editor Begin/End — popups opened
     * inside the node-editor canvas do not receive input).
     */
    class LUX_FUNCTION_UI_PUBLIC SearchPopupElement : public UIElement
    {
    public:
        using SelectCallback = std::function<void(std::uint32_t index)>;

        explicit SearchPopupElement(std::string label = "Search")
            : UIElement(std::move(label)) {}

        /// Searchable display names. Item i -> callback index i. Rebuilds the fuzzy
        /// index (cheap) — call when the source list changes (e.g. on open()).
        void setItems(const std::vector<std::string>& names)
        {
            names_ = names;
            index_.build(names);
        }
        void setOnSelect(SelectCallback cb) { on_select_ = std::move(cb); }
        void setHint(std::string hint)      { hint_ = std::move(hint); }

        /// Request the popup to open on the next draw() (clears the query).
        /// FIXED MODE: the popup anchors AT THE MOUSE (captured here, at the
        /// click that opened it — draw() may run later in the frame from a
        /// different window) and is clamped to the viewport so it never opens
        /// off-screen. Falls back to viewport-center when no mouse position
        /// is available (e.g. opened programmatically).
        void open()
        {
            want_open_    = true;
            query_buf_[0] = '\0';
            open_pos_     = ImGui::GetMousePos();
            has_open_pos_ = open_pos_.x > -FLT_MAX * 0.5f
                         && open_pos_.y > -FLT_MAX * 0.5f;
        }

        void draw() override
        {
            const char* id = label().c_str();
            if (want_open_)
            {
                ImGui::OpenPopup(id);
                want_open_ = false;
            }

            const ImVec2 size{ 380.0f, 420.0f };
            if (has_open_pos_)
            {
                // Clamp so the whole popup stays inside the viewport work area.
                const ImGuiViewport* vp = ImGui::GetMainViewport();
                ImVec2 pos = open_pos_;
                pos.x = std::min(std::max(pos.x, vp->WorkPos.x),
                                 vp->WorkPos.x + vp->WorkSize.x - size.x);
                pos.y = std::min(std::max(pos.y, vp->WorkPos.y),
                                 vp->WorkPos.y + vp->WorkSize.y - size.y);
                ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
            }
            else
            {
                const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
                ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            }
            ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);

            if (!ImGui::BeginPopup(id))
                return;

            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##q", hint_.c_str(), query_buf_, sizeof(query_buf_));

            ImGui::Separator();
            ImGui::BeginChild("##results", ImVec2(0.0f, 0.0f), false);

            const std::vector<FuzzySearchIndex::Match>& matches =
                index_.query(query_buf_, 500);   // bound the per-keystroke sort

            ImGuiListClipper clipper;             // virtualise: render only visible rows
            clipper.Begin(static_cast<int>(matches.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const std::uint32_t idx = matches[static_cast<std::size_t>(row)].index;
                    if (idx >= names_.size()) continue;
                    ImGui::PushID(static_cast<int>(idx));
                    if (ImGui::Selectable(names_[idx].c_str()))
                    {
                        if (on_select_) on_select_(idx);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopID();
                }
            }
            if (matches.empty())
                ImGui::TextDisabled("no match");

            ImGui::EndChild();
            ImGui::EndPopup();
        }

    private:
        FuzzySearchIndex         index_;
        std::vector<std::string> names_;
        char                     query_buf_[256]{};
        std::string              hint_{ "search..." };
        SelectCallback           on_select_;
        bool                     want_open_{ false };
        ImVec2                   open_pos_{ 0.0f, 0.0f }; ///< mouse at open()
        bool                     has_open_pos_{ false };
    };

} // namespace lux::ui
