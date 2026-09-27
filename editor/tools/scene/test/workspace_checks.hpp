#pragma once
#include <lux/engine/editor/WorkspaceRequest.hpp>
#include <lux/engine/editor/metadata/PaneState.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <fstream>

namespace workspace_checks
{
    using namespace lux;
    using namespace lux::editor;
    class Window final : public lux::ui::Pane
    {
    public:
        Window(lux::ui::Root& root, lux::ui::PaneId id, std::string value)
            : Pane(root, std::move(id), lux::ui::PaneTypeId{"test.workspace"}, "Workspace probe"),
              value(std::move(value)), child(
                                           *this,
                                           lux::ui::PaneId{std::string(this->id().name()) + "/child"},
                                           lux::ui::PaneTypeId{"test.auxiliary"},
                                           "Auxiliary probe"
                                       )
        {}
        std::string value;
        lux::ui::Pane child;
    };
    template <class Until> void run(Editor& editor, Until until)
    {
        auto& context = editor.context();
        const auto idle = [&] {
            WorkspaceRequest request;
            assert(object::sendEvent(editor, request));
            return !request.pending;
        };
        until(idle);
        std::vector<PaneRegistration> registrations(
            context.panes().registrations().begin(),
            context.panes().registrations().end()
        );
        PaneRegistration registered;
        registered.type = lux::ui::PaneTypeId{"test.workspace"};
        registered.name = "Workspace probe";
        registered.create = [](PaneManager& panes) noexcept -> PaneRegistration::CreateResult {
            return panes.adopt(std::make_unique<Window>(panes.root(), panes.makeId(), "original"));
        };
        registered.capture = [](PaneManager&, const lux::ui::Pane& pane) noexcept -> EditorResult<std::string> {
            return static_cast<const Window&>(pane).value;
        };
        registered.restore = [](PaneManager& panes, const PaneState& state) noexcept -> PaneRegistration::CreateResult {
            if (auto* existing = panes.find(state.id.view()))
                return std::ref(*existing);
            return panes.adopt(std::make_unique<Window>(panes.root(), state.id, state.payload));
        };
        registrations.push_back(registered);
        assert(context.panes().setRegistrations(std::move(registrations)));
        const auto first = context.panes().create(registered.type.view());
        const auto second = context.panes().create(registered.type.view());
        assert(first && second && first->get().id() != second->get().id());
        const auto first_id = first->get().id(), second_id = second->get().id();
        second->get().setVisible(false);
        const auto operation = [&](EWorkspaceAction action, std::string name, std::string new_name = {}) {
            WorkspaceRequest request;
            request.action = action;
            request.name = std::move(name);
            request.new_name = std::move(new_name);
            assert(object::sendEvent(editor, request) && request.result);
            until(idle);
        };
        operation(EWorkspaceAction::SAVE, "round-trip");
        const auto directory = context.project().root() / ".lux/editor/layouts";
        assert(std::filesystem::exists(directory / "round-trip.toml"));
        // Add an unavailable plugin record using the actual on-disk format.
        {
            std::ofstream file(directory / "round-trip.toml", std::ios::app);
            file << "\n[[panes]]\ntype = 'test.missing-plugin'\nid = 'missing/1'\nvisible = false\npayload = "
                    "'opaque-v9'\n";
            assert(file);
        }
        static_cast<Window&>(first->get()).value = "unsaved content";
        static_cast<Window&>(first->get()).child.setVisible(false);
        assert(context.panes().erase(second_id.view()));
        const auto extra = context.panes().create(registered.type.view());
        assert(extra);
        const auto extra_id = extra->get().id();
        operation(EWorkspaceAction::APPLY, "round-trip");
        assert(static_cast<Window*>(context.panes().find(first_id.view()))->value == "unsaved content");
        assert(static_cast<Window*>(context.panes().find(first_id.view()))->child.visible());
        assert(context.panes().find(extra_id.view()));
        const auto* restored = static_cast<Window*>(context.panes().find(second_id.view()));
        assert(restored && restored->value == "original" && !restored->visible());
        operation(EWorkspaceAction::SAVE, "preserved");
        {
            std::ifstream file(directory / "preserved.toml");
            const std::string bytes{std::istreambuf_iterator<char>{file}, {}};
            assert(bytes.find("opaque-v9") != std::string::npos);
        }
        operation(EWorkspaceAction::RENAME, "preserved", "renamed");
        assert(std::filesystem::exists(directory / "renamed.toml"));
        operation(EWorkspaceAction::REMOVE, "renamed");
        assert(!std::filesystem::exists(directory / "renamed.toml"));
        operation(EWorkspaceAction::DEFAULT, {});
        assert(context.panes().find(first_id.view()) && context.panes().find(extra_id.view()));
        // Freeze a menu target and invalidate it before the queued action is adopted.
        lux::ui::MenuRequest opened{lux::ui::EMenuAction::OPEN, context.panes().find(first_id.view())};
        assert(object::sendEvent(editor, opened));
        lux::ui::MenuRequest close{lux::ui::EMenuAction::COMMAND};
        close.command = {lux::ui::CommandIdView{"lux.window.close"}, lux::ui::ECommandPhase::EXECUTE};
        assert(object::sendEvent(editor, close));
        assert(context.panes().erase(first_id.view()));
        until([&] { return true; });
        assert(context.panes().find(extra_id.view()));
        assert(context.panes().erase(second_id.view()) && context.panes().erase(extra_id.view()));
        std::puts("PASS workspace: multi-instance restore, unsaved/extra windows, unknown plugin, rename/remove, stale "
                  "menu target");
    }
}
