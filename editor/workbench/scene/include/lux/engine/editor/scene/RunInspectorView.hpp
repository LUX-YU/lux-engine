#pragma once
#include <lux/engine/editor/scene/RunInspectorFields.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>

namespace lux::editor::scene
{
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
        RunInspectorView(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            RunStore&,
            simulation::ecs::ComponentSchemaSet,
            std::vector<RunInspectorComponent>,
            project::ProjectCatalogModel* = {}
        );
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
        struct Impl;
        std::unique_ptr<Impl> impl_;
        void update() noexcept override;
    };
    [[nodiscard]] RunResult<views::DetachedView> makeRunInspectorView(
        object::ObjectDispatcherRef,
        lux::ui::PaneId,
        RunStore&,
        RunningObjectRef,
        simulation::ecs::ComponentSchemaSet,
        std::vector<RunInspectorComponent>,
        project::ProjectCatalogModel* = {}
    );
}
