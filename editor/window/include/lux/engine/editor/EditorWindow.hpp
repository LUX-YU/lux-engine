#pragma once
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/window/LuxWindow.hpp>

namespace lux::ui
{
    class Root;
}
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
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorWindow>>
        create(const window::InitParameter&) noexcept;
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
        [[nodiscard]] const WindowMetrics& metrics() const noexcept
        {
            return metrics_;
        }

    private:
        // These backend slots belong to the metrics provider, not product assembly.
        using LuxWindow::on_framebuffer_resize;
        using LuxWindow::on_minimized;
        using LuxWindow::on_resize;
        explicit EditorWindow(const window::InitParameter&);
        void metricsChanged() noexcept;
        WindowMetrics metrics_;
        input::Input input_;
        std::unique_ptr<ui::Root> root_;
    };
} // namespace lux::editor
