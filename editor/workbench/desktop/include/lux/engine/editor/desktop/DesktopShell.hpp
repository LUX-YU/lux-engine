#pragma once
#include <lux/engine/editor/desktop/CommandMenu.hpp>
#include <lux/engine/editor/desktop/Presentation.hpp>
#include <optional>

namespace lux::input
{
    struct InputSnapshot;
}
namespace lux::editor::desktop
{
    // Native window, execution, renderer/resources and the single SceneRuntime frame owner are borrowed.
    // They outlive this shell and drain its retired UI scene before destroying the native surface.
    class DesktopShell final
    {
    public:
        [[nodiscard]] static DesktopResult<std::unique_ptr<DesktopShell>> create(
            object::ObjectDispatcherRef,
            process::ExecutionRuntime&,
            lux::scene::SceneRuntime&,
            render::RenderRuntime&,
            lux::scene::RenderResources&,
            window::LuxWindow* = nullptr,
            lux::ui::RootConfig = {}
        );
        ~DesktopShell() noexcept;
        DesktopShell(const DesktopShell&) = delete;
        DesktopShell& operator=(const DesktopShell&) = delete;
        DesktopShell(DesktopShell&&) = delete;
        DesktopShell& operator=(DesktopShell&&) = delete;
        [[nodiscard]] commands::CommandResult<void> installCommands(
            commands::CommandRegistry&,
            commands::CommandDispatcher&,
            CommandMenu::Capture
        );
        [[nodiscard]] CommandMenu* commands() noexcept;
        [[nodiscard]] lux::ui::Root& root() noexcept;
        [[nodiscard]] Presentation& presentation() noexcept;
        [[nodiscard]] DesktopResult<void> feedInput(const input::InputSnapshot&) noexcept;
        // Native size when a window is present; explicit extent enables the same production desktop in
        // offscreen harnesses. No UI frame available still advances owner maintenance and pending closes.
        [[nodiscard]] DesktopResult<void> update(std::optional<lux::ui::FrameInfo> = {});

    private:
        struct Impl;
        explicit DesktopShell(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::desktop
