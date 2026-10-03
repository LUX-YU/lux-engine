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
namespace lux::editor::views
{
    class ViewFactorySnapshot;
}
namespace lux::editor::desktop
{
    class ViewHost;
    // A short Host transaction, followed by an independent preferences publication. The caller
    // holds the existing contribution batch protection while lending the factory snapshot.
    class WorkspaceActions final
    {
    public:
        WorkspaceActions(
            ViewHost&,
            workspace::WorkspaceStore&,
            workspace::WorkspaceChanges&,
            object::ObjectDispatcherRef
        );
        ~WorkspaceActions();
        WorkspaceActions(const WorkspaceActions&) = delete;
        WorkspaceActions& operator=(const WorkspaceActions&) = delete;
        WorkspaceActions(WorkspaceActions&&) = delete;
        WorkspaceActions& operator=(WorkspaceActions&&) = delete;

        [[nodiscard]] EditorResult<void> save(std::string label);
        [[nodiscard]] EditorResult<void> apply(const workspace::LayoutId&, const views::ViewFactorySnapshot&);
        [[nodiscard]] EditorResult<void> apply(workspace::DockLayout, const views::ViewFactorySnapshot&);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::desktop
