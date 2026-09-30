#pragma once
#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/editor/scene/SceneSessionAccess.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>

namespace lux::editor::scene
{
    class SceneConfigurationView final : public lux::ui::Pane
    {
    public:
        SceneConfigurationView(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            SceneSessionAccess,
            SceneConfigurationInputs
        );
        ~SceneConfigurationView() noexcept override;
        SceneConfigurationView(const SceneConfigurationView&) = delete;
        SceneConfigurationView& operator=(const SceneConfigurationView&) = delete;
        SceneConfigurationView(SceneConfigurationView&&) = delete;
        SceneConfigurationView& operator=(SceneConfigurationView&&) = delete;
        [[nodiscard]] SceneConfigurationResult<void> rebind(sessions::TSessionKey<SceneSession>);
        [[nodiscard]] SceneConfigurationResult<void> prepareClose();
        [[nodiscard]] SceneConfigurationElement* form() noexcept;
        void requestApply() noexcept;
        void requestRevert() noexcept;
        [[nodiscard]] const SceneConfigurationResult<void>& status() const noexcept;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] SceneConfigurationResult<views::DetachedView>
        makeSceneConfigurationView(object::ObjectDispatcherRef, lux::ui::PaneId, SceneSessionAccess, SceneConfigurationInputs, sessions::TSessionKey<SceneSession>);
}
