#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/workspace/DockLayout.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <memory>

namespace lux::editor::workspace
{
    class WorkspaceStore;
    class WorkspaceChanges;
} // namespace lux::editor::workspace
namespace lux::ui
{
    class Root;
}
namespace lux::services
{
    class ServiceScope;
}
namespace lux::editor::desktop
{
    class UiRegistry;
    // A short Root transaction, followed by an independent preferences publication. The caller
    // holds the existing contribution batch protection while lending the registered UI factories.
    class WorkspaceActions final
    {
    public:
        WorkspaceActions(lux::ui::Root&, UiRegistry&, services::ServiceScope&, workspace::WorkspaceStore&, workspace::WorkspaceChanges&);
        ~WorkspaceActions();
        WorkspaceActions(const WorkspaceActions&) = delete;
        WorkspaceActions& operator=(const WorkspaceActions&) = delete;
        WorkspaceActions(WorkspaceActions&&) = delete;
        WorkspaceActions& operator=(WorkspaceActions&&) = delete;

        [[nodiscard]] EditorResult<void> save(std::string label);
        [[nodiscard]] EditorResult<void> apply(const workspace::LayoutId&);
        [[nodiscard]] EditorResult<void> apply(workspace::DockLayout);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::desktop
