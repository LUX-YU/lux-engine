#pragma once
//
// ToastQueue — minimal transient notification overlay (tech-debt §1.14).
//
// Operations (import, save, delete) used to report only to stderr, invisible
// to the user inside the editor. ToastQueue collects short messages and paints
// them as a stack of fading panels in the bottom-right corner. It is driven by
// a UISystem overlay hook (see UISystem::setOverlayHook), so it floats above
// all docked panels.
//
// Threading: push() and paint() are expected to run on the main/UI thread
// (push from the editor's drained pending-action callbacks, paint from the
// overlay hook inside UISystem::newFrame). No internal locking.
//
#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

namespace lux::editor
{
    enum class ToastLevel
    {
        Info,
        Success,
        Warning,
        Error
    };

    class ToastQueue
    {
    public:
        /// Queue a message for @p seconds. Uses ImGui time, so it must be
        /// called within an active ImGui frame (true for drained UI actions).
        void push(std::string message,
                  ToastLevel  level   = ToastLevel::Info,
                  float       seconds = 4.0f)
        {
            const float now = static_cast<float>(ImGui::GetTime());
            toasts_.push_back(Toast{ std::move(message), level, now + seconds, seconds });
        }

        /// Paint + expire. Call once per frame from a UISystem overlay hook.
        void paint()
        {
            const float now = static_cast<float>(ImGui::GetTime());
            std::erase_if(toasts_,
                [now](const Toast& t) { return now >= t.expire; });
            if (toasts_.empty())
                return;

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float pad = 12.0f;

            // Stack upward from the bottom-right corner; newest on top.
            float y = vp->WorkPos.y + vp->WorkSize.y - pad;
            const float x = vp->WorkPos.x + vp->WorkSize.x - pad;

            const ImGuiWindowFlags flags =
                ImGuiWindowFlags_NoDecoration  | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoNav         | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoDocking;

            for (std::size_t i = toasts_.size(); i-- > 0; )
            {
                const Toast& t = toasts_[i];

                // Fade out over the final 0.6s of the toast's lifetime.
                const float remaining = t.expire - now;
                const float alpha = std::clamp(remaining / 0.6f, 0.0f, 1.0f);

                ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
                ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 0.0f), ImVec2(440.0f, FLT_MAX));
                ImGui::SetNextWindowBgAlpha(0.88f * alpha);
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

                const std::string win_id = std::format("##lux_toast_{}", i);
                if (ImGui::Begin(win_id.c_str(), nullptr, flags))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, colorFor(t.level));
                    ImGui::PushTextWrapPos(420.0f);
                    ImGui::TextUnformatted(t.text.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();

                    y -= ImGui::GetWindowHeight() + 8.0f;
                }
                ImGui::End();
                ImGui::PopStyleVar();
            }
        }

        void clear() { toasts_.clear(); }

    private:
        struct Toast
        {
            std::string text;
            ToastLevel  level;
            float       expire;   ///< ImGui-time at which it disappears.
            float       total;    ///< Original lifetime (seconds).
        };

        static ImVec4 colorFor(ToastLevel level)
        {
            switch (level)
            {
            case ToastLevel::Success: return ImVec4(0.55f, 0.90f, 0.55f, 1.0f);
            case ToastLevel::Warning: return ImVec4(0.97f, 0.82f, 0.40f, 1.0f);
            case ToastLevel::Error:   return ImVec4(0.97f, 0.45f, 0.45f, 1.0f);
            case ToastLevel::Info:
            default:                  return ImVec4(0.90f, 0.90f, 0.92f, 1.0f);
            }
        }

        std::vector<Toast> toasts_;
    };
}
