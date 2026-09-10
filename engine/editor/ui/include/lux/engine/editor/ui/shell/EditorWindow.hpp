#pragma once

#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/editor/ui/actions/ActiveEditHistory.hpp>
#include <lux/engine/editor/ui/layout/WorkspaceLayout.hpp>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/ui/UISession.hpp>

namespace lux::window
{
    class LuxWindow;
}

namespace lux::editor::ui
{
    class LUX_EDITOR_UI_PUBLIC EditorWindow final : public lux::object::Object<EditorWindow>
    {
    public:
        [[nodiscard]] static WindowResult<std::unique_ptr<EditorWindow>> create(lux::object::ObjectDispatcherRef,
                                                                                const WindowSpec &) noexcept;
        ~EditorWindow() noexcept override;
        EditorWindow(const EditorWindow &) = delete;
        EditorWindow &operator=(const EditorWindow &) = delete;
        EditorWindow(EditorWindow &&) = delete;
        EditorWindow &operator=(EditorWindow &&) = delete;

        [[nodiscard]] lux::ui::UISession &uiSession() noexcept;
        [[nodiscard]] lux::window::LuxWindow &nativeWindow() noexcept;
        [[nodiscard]] ActiveEditHistory &activeHistory() noexcept;
        [[nodiscard]] WindowResult<void> collectInput() noexcept;
        [[nodiscard]] WindowResult<void> beginFrame(const lux::ui::FrameInfo &) noexcept;
        [[nodiscard]] WindowResult<void> drawPanes() noexcept;
        [[nodiscard]] WindowResult<lux::ui::UiFrameSnapshot> finishFrame() noexcept;
        [[nodiscard]] WindowResult<void> discardFrame() noexcept;
        [[nodiscard]] WindowResult<void> installLayout(const WorkspaceLayout &) noexcept;
        [[nodiscard]] WindowResult<void> requestClose() noexcept;
        [[nodiscard]] WindowResult<void> closeAfterRendererStopped() noexcept;
        [[nodiscard]] bool frameOpen() const noexcept;
        [[nodiscard]] bool closeRequested() const noexcept;
        [[nodiscard]] WindowResult<TextInputPlatformStatus> textInputPlatformStatus() const noexcept;

    private:
        struct Impl;
        EditorWindow(lux::object::ObjectDispatcherRef, std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::ui
