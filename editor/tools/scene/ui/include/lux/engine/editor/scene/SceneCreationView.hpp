#pragma once
#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>

namespace lux::editor::scene
{
    struct SceneCreationRequests final
    {
        // The application creates/adopts the new Session. Rejection does not consume the form.
        std::function<views::ViewResult<void>(const SceneCreationConfiguration&)> create;
    };
    class SceneCreationView final : public lux::ui::Pane
    {
    public:
        SceneCreationView(object::ObjectDispatcherRef, lux::ui::PaneId, SceneConfigurationInputs, SceneCreationRequests, SceneConfigurationResult<void>&);
        ~SceneCreationView() noexcept override;
        SceneCreationView(const SceneCreationView&) = delete;
        SceneCreationView& operator=(const SceneCreationView&) = delete;
        SceneCreationView(SceneCreationView&&) = delete;
        SceneCreationView& operator=(SceneCreationView&&) = delete;
        [[nodiscard]] SceneConfigurationElement& configuration() noexcept;
        void requestCreate() noexcept;
        [[nodiscard]] const SceneConfigurationResult<void>& status() const noexcept;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] SceneConfigurationResult<views::DetachedView> makeSceneCreationView(
        object::ObjectDispatcherRef,
        lux::ui::PaneId,
        SceneConfigurationInputs,
        SceneCreationRequests
    );
}
