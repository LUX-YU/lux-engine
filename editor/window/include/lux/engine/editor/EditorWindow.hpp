#pragma once
#include <chrono>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/window/LuxWindow.hpp>

namespace lux::ui
{
    class Root;
    class DrawData;
} // namespace lux::ui

namespace lux::editor
{
    struct WindowMetrics final
    {
        std::uint32_t width{}, height{};
        std::uint32_t framebuffer_width{}, framebuffer_height{};
        std::uint64_t revision{1};
        bool minimized{};
    };

    class EditorWindow final : public window::LuxWindow
    {
    public:
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorWindow>> create(const window::
                                                                                       InitParameter&) noexcept;
        ~EditorWindow() override;
        EditorWindow(const EditorWindow&) = delete;
        EditorWindow& operator=(const EditorWindow&) = delete;
        EditorWindow(EditorWindow&&) = delete;
        EditorWindow& operator=(EditorWindow&&) = delete;
        [[nodiscard]] ui::Root& uiRoot() noexcept;

        [[nodiscard]] input::Input& input() noexcept
        {
            return input_;
        }

        [[nodiscard]] FrameworkResult<void> sampleInput() noexcept;
        // Exclusive borrowed receiver; registration folds in current metrics. Clear it before receiver destruction.
        void setMetricsReceiver(void*, void (*changed)(void*, const WindowMetrics&) noexcept) noexcept;
        // CPU UI and input only. The caller supplies this frame's storage; no Render dependency lives here.
        [[nodiscard]] FrameworkResult<void> updateUi(ui::DrawData&) noexcept;
        [[nodiscard]] FrameworkResult<void> maintainUi() noexcept;

        [[nodiscard]] const WindowMetrics& metrics() const noexcept
        {
            return metrics_;
        }

    private:
        // These backend slots belong to the metrics provider, not product assembly.
        using LuxWindow::on_framebuffer_resize;
        using LuxWindow::on_minimized;
        using LuxWindow::on_resize;
        EditorWindow(NativeWindowOwner, std::unique_ptr<ui::Root>) noexcept;
        void metricsChanged() noexcept;
        [[nodiscard]] FrameworkResult<void> updateUiFrame(ui::DrawData*) noexcept;
        WindowMetrics metrics_;
        void* metrics_receiver_ = nullptr;
        void (*metrics_changed_)(void*, const WindowMetrics&) noexcept = nullptr;
        std::chrono::steady_clock::time_point last_ui_frame_{std::chrono::steady_clock::now()};
        input::Input input_;
        std::unique_ptr<ui::Root> root_;
    };
} // namespace lux::editor
