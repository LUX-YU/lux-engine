#include <lux/engine/editor/project/ResultsView.hpp>
#include <lux/engine/editor/project/WorkspaceView.hpp>
#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <cassert>
#include <iostream>
#include "ProjectToolFactories.hpp"

int main(int argc, char** argv)
{
    assert(argc == 2);
    projectToolFactories(argv[1]);
    using namespace lux;
    using namespace lux::editor;
    auto messages = object::ObjectMessageQueue::create(32);
    assert(messages);
    auto root = ui::Root::create(messages->dispatcherRef());
    assert(root);
    bool unavailable{};
    std::optional<project::VResultIntent> received;
    std::optional<project::VWorkspaceIntent> workspace_received;
    const sessions::ContentStamp target{{UINT64_MAX - 2, 8, UINT64_MAX - 1}, {}};
    project::ResultsView results(messages->dispatcherRef(), ui::PaneId{"results"},
        [&]() -> EditorResult<project::ResultsSnapshot> {
            if (unavailable)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "test.busy"});
            return project::ResultsSnapshot{{{"Contents", {{"full-identity", {"Original display"},
                {{"Show", project::ShowContent{target}}}}}}}};
        },
        [&](project::VResultIntent intent) -> EditorResult<void> {
            if (received)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "test.capacity"});
            received = std::move(intent);
            return {};
        }
    );
    project::WorkspaceView workspace(messages->dispatcherRef(), ui::PaneId{"workspace"},
        [&]() -> EditorResult<project::WorkspaceSnapshot> {
            if (unavailable)
                return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "test.io"});
            project::WorkspaceSnapshot value;
            value.catalog.layouts.push_back({{"stable-layout"}, "Label", "version"});
            return value;
        },
        [&](project::VWorkspaceIntent intent) -> EditorResult<void> {
            if (workspace_received)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "test.capacity"});
            workspace_received = std::move(intent);
            return {};
        }
    );
    assert(!results.attachedRoot() && !workspace.attachedRoot());
    ui_test::mount(**root, results);
    ui_test::mount(**root, workspace);
    const auto frame = [&] {
        ui::DrawData data;
        assert((*root)->update({{900, 700}, 1.0F / 60}, &data));
    };
    frame();
    assert(workspace_received && std::holds_alternative<project::RefreshWorkspace>(*workspace_received));
    assert(results.snapshot().sections.front().rows.front().messages.front() == "Original display");
    const auto action = results.snapshot().sections.front().rows.front().actions.front().intent;
    assert(results.request(action));
    assert(std::get<project::ShowContent>(*received).target == target);
    assert(!results.request(project::AcknowledgeMaintenance{}));
    assert(std::get<project::ShowContent>(*received).target == target);
    unavailable = true;
    frame();
    assert(results.observationFailure() && workspace.observationFailure());
    assert(results.snapshot().sections.front().rows.front().messages.front() == "Original display");
    assert(workspace.snapshot().catalog.layouts.front().id.value == "stable-layout");
    workspace_received.reset();
    assert(workspace.request(project::ApplyLayout{{"stable-layout"}}));
    assert(!workspace.request(project::RemoveLayout{{"different-layout"}}));
    assert(std::get<project::ApplyLayout>(*workspace_received).layout.value == "stable-layout");
    unavailable = false;
    frame();
    assert(!results.observationFailure() && !workspace.observationFailure());
    std::cout << "EC2 independent panels: owning display, BUSY/IO retention, full typed targets, bounded receiver\n";
}
