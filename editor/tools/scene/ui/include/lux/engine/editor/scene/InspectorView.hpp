#pragma once
#include <lux/engine/editor/scene/InspectorFields.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>

namespace lux::editor::scene
{
    struct InspectorComponent final
    {
        using CreateResult = SceneEditResult<std::unique_ptr<lux::ui::Element>>;
        cxx::TypeToken type;
        std::string label;
        CreateResult (*create)(lux::ui::Element&, lux::ui::ElementId, InspectorFields&){};
        std::shared_ptr<const void> code;
    };
    [[nodiscard]] std::vector<InspectorComponent> sceneInspectorComponents();

    class InspectorView final : public lux::ui::Pane
    {
    public:
        InspectorView(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            SceneSessionAccess,
            simulation::ecs::ComponentSchemaSet,
            std::vector<InspectorComponent>,
            project::ProjectCatalogAccess = {}
        );
        ~InspectorView() noexcept override;
        InspectorView(const InspectorView&) = delete;
        InspectorView& operator=(const InspectorView&) = delete;
        InspectorView(InspectorView&&) = delete;
        InspectorView& operator=(InspectorView&&) = delete;
        [[nodiscard]] SceneEditResult<void> rebind(EditedSceneBinding, SceneObjectRef);
        [[nodiscard]] SceneEditResult<void> finishEditing();
        [[nodiscard]] SceneEditResult<void> addComponent(const simulation::ecs::ComponentSchemaId&);
        [[nodiscard]] SceneEditResult<void> removeComponent(const simulation::ecs::ComponentSchemaId&);
        [[nodiscard]] SceneEditResult<void> prepareClose();
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
        SceneSessionAccess,
        EditedSceneBinding,
        SceneObjectRef,
        simulation::ecs::ComponentSchemaSet,
        std::vector<InspectorComponent>,
        project::ProjectCatalogAccess = {}
    );
}
