#pragma once
#include <lux/engine/editor/scene/InspectorFields.hpp>
#include <lux/engine/editor/scene/InspectorComponent.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>

namespace lux::editor::scene
{
    class SceneInteractionGroup;
    [[nodiscard]] std::vector<InspectorComponent> sceneInspectorComponents();

    class InspectorView final : public lux::ui::Pane
    {
    public:
        InspectorView(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            sessions::TSessionAccess<SceneSession>,
            simulation::ecs::ComponentSchemaSet,
            std::vector<InspectorComponent>,
            project::ProjectCatalogModel* = {},
            std::shared_ptr<SceneInteractionGroup> = {}
        );
        [[nodiscard]] const std::shared_ptr<SceneInteractionGroup>& interactionOwner() const noexcept;
        ~InspectorView() noexcept override;
        InspectorView(const InspectorView&) = delete;
        InspectorView& operator=(const InspectorView&) = delete;
        InspectorView(InspectorView&&) = delete;
        InspectorView& operator=(InspectorView&&) = delete;
        [[nodiscard]] SceneEditResult<void> rebind(EditedSceneBinding, SceneObjectRef);
        [[nodiscard]] SceneEditResult<void> clearTarget();
        [[nodiscard]] SceneEditResult<void> finishEditing();
        [[nodiscard]] SceneEditResult<void> cancelEditing();
        [[nodiscard]] SceneEditResult<void> addComponent(const simulation::ecs::ComponentSchemaId&);
        [[nodiscard]] SceneEditResult<void> removeComponent(const simulation::ecs::ComponentSchemaId&);
        [[nodiscard]] SceneEditResult<void> prepareClose();
        [[nodiscard]] views::ViewContent content() const noexcept;
        [[nodiscard]] std::optional<SceneObjectRef> target() const noexcept;
        [[nodiscard]] const SceneEditResult<void>& status() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        void update() noexcept override;
    };
    [[nodiscard]] SceneViewResult<views::DetachedView> makeInspectorView(
        object::ObjectDispatcherRef,
        lux::ui::PaneId,
        sessions::TSessionAccess<SceneSession>,
        EditedSceneBinding,
        SceneObjectRef,
        simulation::ecs::ComponentSchemaSet,
        std::vector<InspectorComponent>,
        project::ProjectCatalogModel* = {},
        std::shared_ptr<SceneInteractionGroup> = {}
    );
}
