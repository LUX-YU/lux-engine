#pragma once
#include <lux/engine/editor/scene/RunInspectorFields.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::scene
{
    class SceneInteractionGroup;
    struct RunInspectorComponent final
    {
        using CreateResult = RunResult<std::unique_ptr<lux::ui::Element>>;
        cxx::TypeToken type;
        std::string label;
        CreateResult (*create)(lux::ui::Element&, lux::ui::ElementId, RunInspectorFields&){};
        RunInspectorFields::Copy copy{};
        std::shared_ptr<const void> code;
    };
    [[nodiscard]] std::vector<RunInspectorComponent> runInspectorComponents();
    class RunInspectorView final : public lux::ui::Pane
    {
    public:
        [[nodiscard]] static RunResult<std::unique_ptr<RunInspectorView>> create(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            std::shared_ptr<RunStore>,
            simulation::ecs::ComponentSchemaSet,
            std::vector<RunInspectorComponent>,
            project::ProjectCatalogModel* = {},
            std::shared_ptr<SceneInteractionGroup> = {}
        );
        [[nodiscard]] const std::shared_ptr<SceneInteractionGroup>& interactionOwner() const noexcept;
        ~RunInspectorView() noexcept override;
        RunInspectorView(const RunInspectorView&) = delete;
        RunInspectorView& operator=(const RunInspectorView&) = delete;
        RunInspectorView(RunInspectorView&&) = delete;
        RunInspectorView& operator=(RunInspectorView&&) = delete;
        [[nodiscard]] RunResult<void> rebind(RunningObjectRef);
        [[nodiscard]] RunResult<void> clearTarget();
        [[nodiscard]] RunResult<void> finishEditing();
        [[nodiscard]] RunResult<void> cancelEditing();
        [[nodiscard]] RunResult<void> prepareClose();
        [[nodiscard]] std::optional<RunningObjectRef> target() const noexcept;
        [[nodiscard]] const RunResult<void>& status() const noexcept;

    private:
        RunInspectorView(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            std::shared_ptr<RunStore>,
            simulation::ecs::ComponentSchemaSet,
            std::vector<RunInspectorComponent>,
            project::ProjectCatalogModel*,
            std::shared_ptr<SceneInteractionGroup>
        );
        struct Impl;
        std::unique_ptr<Impl> impl_;
        void update() noexcept override;
    };
}
