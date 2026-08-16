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
#include <cstdint>
#include <lux/engine/platform/FormatCompat.h>
#include <string>
#include <string_view>
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

        /// 当前在屏/待显示条数(错误洪水保护的判据 —— 见 LuxEditor 的
        /// LogRecord ui 泵订阅)。
        [[nodiscard]] std::size_t size() const noexcept { return toasts_.size(); }

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

                const std::string win_id = lux::format("##lux_toast_{}", i);
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

    // (这里曾有 `LogToastSink` —— lux::log 的 Error 级编辑器可见出口。它的两段
    //  缓冲、以及那一整页「为什么主线程独占不加锁」的逐调用点论证,存在的根因是
    //  「LogSink::write 发生在记日志的那一刻,可能在任何线程任何时机」。日志收编
    //  进统一事件总线(事件批C)后,这个前提被结构性消解:LogRecord 经无锁队列
    //  到 ui 泵,handler 恒在 UI 线程的 ImGui 帧内执行 —— 直接 push 进 ToastQueue
    //  即可。现场见 LuxEditor::init 的 LogRecord ui 泵订阅。)
}
