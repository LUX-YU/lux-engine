#pragma once

#include <filesystem>
#include <functional>
#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/ui/CommandRouter.hpp>
#include <lux/engine/ui/Context.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Theme.hpp>
#include <lux/engine/ui/UiInputEvent.hpp>
#include <lux/engine/ui/UiTextInputAnchor.hpp>

namespace lux::editor::ui
{
namespace detail
{
struct PaneControl;
}
class UIRenderSystem;

enum class EUiRegistrationError
{
    DUPLICATE_PANE_ID,
    INVALID_ID,
    FOREIGN_DISPATCHER
};

struct UIRenderSystemConfig final
{
    lux::ui::Theme theme{lux::ui::Theme::luxDark()};
    bool docking{true};
    const lux::ui::UiFontSource *font{}; // Cold input, copied by Context before installation returns.
    std::function<EditorResult<bool>(std::filesystem::path &)> select_existing_file;
};

class LUX_EDITOR_UI_PUBLIC PaneRegistration final
{
  public:
    PaneRegistration() noexcept = default;
    PaneRegistration(const PaneRegistration &) = delete;
    PaneRegistration &operator=(const PaneRegistration &) = delete;
    PaneRegistration(PaneRegistration &&) noexcept;
    PaneRegistration &operator=(PaneRegistration &&) noexcept;
    ~PaneRegistration();
    void reset() noexcept;
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return token_ != 0;
    }

  private:
    friend class UIRenderSystem;
    PaneRegistration(std::weak_ptr<detail::PaneControl>, std::uint64_t) noexcept;
    std::weak_ptr<detail::PaneControl> control_;
    std::uint64_t token_{};
};

class LUX_EDITOR_UI_PUBLIC LUX_OBJECT() UIRenderSystem final
    : public object::Object<UIRenderSystem, lux::scene::RenderSystem>
{
  public:
    inline static constexpr std::array Capabilities{std::string_view{"lux.scene.render"}};
    inline static constexpr system::SystemTypeDescription Description{
        .canonical_name = "lux.editor.system.ui",
        .version = 1,
        .capabilities = Capabilities,
        .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER};
    ~UIRenderSystem() noexcept override;
    static const signal_type<asset::AssetId> assetOpenRequested;
    void openAsset(asset::AssetId);
    [[nodiscard]] EditorResult<bool> selectExistingFile(std::filesystem::path &);
    UIRenderSystem(const UIRenderSystem &) = delete;
    UIRenderSystem &operator=(const UIRenderSystem &) = delete;
    [[nodiscard]] lux::cxx::expected<PaneRegistration, EUiRegistrationError> registerPane(lux::ui::Pane &);
    [[nodiscard]] lux::ui::CommandRouter &commandRouter() noexcept;
    [[nodiscard]] const lux::ui::CommandRouter &commandRouter() const noexcept;
    [[nodiscard]] object::ObjectDispatcherRef dispatcherRef() const noexcept;
    [[nodiscard]] bool requestFocus(lux::ui::PaneIdView);
    [[nodiscard]] lux::ui::Pane *focusedPane() const noexcept;
    [[nodiscard]] std::span<const lux::ui::UiContextIdView> focusedContexts() const noexcept;
    void feedInput(const lux::ui::UiInputEvent &);
    [[nodiscard]] lux::ui::UiInputSnapshot inputSnapshot() const noexcept;
    [[nodiscard]] lux::ui::UiTextInputAnchor textInputAnchor() const noexcept;
    [[nodiscard]] bool canBuildFrame() const noexcept;
    void stopFrames() noexcept;
    [[nodiscard]] lux::ui::Frame beginFrame(lux::ui::FrameInfo);
    void drawPanes(lux::ui::Frame &);
    [[nodiscard]] lux::cxx::expected<void, lux::ui::EUiCaptureError> finishFrame(
        lux::ui::Frame &, std::span<const render::ViewImage> images = {});
    [[nodiscard]] lux::scene::SceneStageResult publishFrame(lux::scene::SceneStageContext &) noexcept;
    [[nodiscard]] std::uint64_t capturedFrames() const noexcept;
    void setSplitLayout(lux::ui::SplitLayout);
    void clearSplitLayout();
    [[nodiscard]] lux::cxx::expected<void, lux::ui::ELayoutError> validateSplitLayout(
        const lux::ui::SplitLayout &) const noexcept;
    [[nodiscard]] lux::ui::LayoutSnapshot captureLayout() const;
    [[nodiscard]] lux::cxx::expected<void, lux::ui::ELayoutError> restoreLayout(const lux::ui::LayoutSnapshot &);

  private:
    friend class lux::scene::SceneBuilder;
    friend class PaneRegistration;
    friend lux::cxx::expected<void, lux::scene::SceneSystemBuildFailure> installUIRenderSystem(
        lux::scene::SceneBuilder &, lux::scene::SceneSystemDescription) noexcept;
    UIRenderSystem(system::SystemInstanceId, lux::scene::SceneInstanceId, render::RenderRuntime &,
                   simulation::ecs::Registry &, render::RenderSceneLease, lux::ui::Context,
                   const UIRenderSystemConfig &, object::ObjectDispatcherRef);
    void initialize();
    [[nodiscard]] lux::scene::SceneStageResult maintainUi() noexcept;
    void unregisterPane(std::uint64_t) noexcept;
    void updateCommandRoute(object::LuxObject *, std::span<const lux::ui::UiContextIdView>);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<detail::PaneControl> control_;
};

[[nodiscard]] LUX_EDITOR_UI_PUBLIC lux::scene::SceneSystemRegistration uiRenderSystemRegistration() noexcept;
} // namespace lux::editor::ui
