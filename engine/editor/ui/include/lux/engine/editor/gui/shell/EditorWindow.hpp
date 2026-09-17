#pragma once

#include <lux/engine/editor/gui/actions/ActiveEditHistory.hpp>
#include <lux/engine/editor/gui/shell/WindowSpec.hpp>
#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/ui/UISession.hpp>

namespace lux::window
{
class LuxWindow;
}

namespace lux::editor::gui
{
class LUX_EDITOR_UI_PUBLIC LUX_OBJECT() EditorWindow final : public lux::object::Object<EditorWindow>
{
  public:
    static const signal_type<asset::AssetId> assetOpenRequested;
    void openAsset(asset::AssetId source);
    // Native owner-modal selection; cancellation returns false and retains the supplied path.
    [[nodiscard]] WindowResult<bool> selectExistingFile(std::filesystem::path &);
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
    [[nodiscard]] WindowResult<void> installLayout(const lux::ui::SplitLayout &) noexcept;
    [[nodiscard]] WindowResult<void> requestClose() noexcept;
    [[nodiscard]] WindowResult<void> closeAfterRendererStopped() noexcept;
    [[nodiscard]] bool frameOpen() const noexcept;
    // Successful CPU snapshot captures. Retries and GPU completion do not advance this count.
    [[nodiscard]] std::uint64_t capturedFrames() const noexcept;
    [[nodiscard]] bool closeRequested() const noexcept;
    void cancelCloseRequest() noexcept;
    [[nodiscard]] WindowResult<TextInputPlatformStatus> textInputPlatformStatus() const noexcept;

  private:
    struct Impl;
    EditorWindow(lux::object::ObjectDispatcherRef, std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace lux::editor::gui
