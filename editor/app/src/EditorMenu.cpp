#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/metadata/PaneState.hpp>
#include <algorithm>
#include <array>
#include <lux/engine/editor/ui/SaveAllRequest.hpp>
#include <cstdio>

namespace lux::editor
{
    namespace
    {
        lux::ui::MenuItem item(std::string id, std::string label, std::string hint = {}, lux::ui::Shortcut key = {})
        {
            return {lux::ui::CommandId{std::move(id)}, std::move(label), std::move(hint), key};
        }
    }
    void Editor::Impl::initializeMenu()
    {
        EditorResult<void> status;
        menu_removed_ = detail::takeConnection(
            object::LuxObject::connect(
                root,
                &lux::ui::Root::objectRemoved,
                [this](object::LuxObject* removed) noexcept {
                    const auto invalidate = [removed](MenuCall& call) noexcept {
                        if (call.pane == removed || call.element == removed)
                        {
                            call.cancelled = true;
                            call.pane = nullptr;
                            call.element = nullptr;
                        }
                    };
                    invalidate(menu_target_);
                    for (auto& request : menu_requests_)
                        invalidate(request);
                }
            ),
            status
        );
        if (!status)
            fail(status.error());
        rebuildMenu();
    }
    void Editor::Impl::reportMenuFailure(const EditorFailure& failure)
    {
        menu_error_ = failure.domain + ": " + failure.message;
        menu_dirty_ = true;
        std::fprintf(stderr, "[editor.command] %s\n", menu_error_.c_str());
    }
    bool Editor::Impl::validMenuTarget(const MenuCall& call) const noexcept
    {
        if (call.cancelled || (call.commands_revision && call.commands_revision != context->commandRevision()))
            return false;
        if (!call.pane)
            return !call.element;
        if (root->findPane(call.pane->id().view()) != call.pane)
            return false;
        if (call.element && &call.element->pane() != call.pane)
            return false;
        editing::HistoryCommand query;
        static_cast<void>(object::routeEvent(*call.pane, *root, query));
        return query.history == call.history;
    }
    void Editor::Impl::receiveMenu(lux::ui::MenuRequest& request) noexcept
    {
        if (request.action == lux::ui::EMenuAction::OPEN)
        {
            menu_target_ = {};
            menu_target_.pane = request.pane;
            menu_target_.element = request.element;
            menu_target_.commands_revision = context->commandRevision();
            if (request.pane)
            {
                editing::HistoryCommand query;
                static_cast<void>(object::routeEvent(*request.pane, *root, query));
                menu_target_.history = query.history;
            }
            return;
        }
        if (request.action != lux::ui::EMenuAction::COMMAND)
            return;
        if (!validMenuTarget(menu_target_))
        {
            request.command.enabled = false;
            return;
        }
        if (request.command.phase == lux::ui::ECommandPhase::QUERY)
        {
            dispatchCommand(menu_target_, request.command);
            return;
        }
        auto call = menu_target_;
        call.command = lux::ui::CommandId{std::string(request.command.id.name())};
        menu_requests_.push_back(std::move(call));
        request.command.result = lux::ui::ECommandDispatchResult::EXECUTED;
    }
    void Editor::Impl::dispatchCommand(const MenuCall& call, lux::ui::Command& command) noexcept
    {
        if (reviewing_exit_ || exit_requested_)
        {
            command.enabled = false;
            command.result = lux::ui::ECommandDispatchResult::DISABLED;
            return;
        }
        object::LuxObject* target = call.element ? static_cast<object::LuxObject*>(call.element) : call.pane;
        const auto* previous = std::exchange(active_command_, &call);
        root_command_ = false;
        if (target)
        {
            static_cast<void>(object::routeEvent(*target, *root, command));
            if (root_command_ && command.phase == lux::ui::ECommandPhase::EXECUTE)
                applicationCommand(command);
        }
        else
            applicationCommand(command);
        active_command_ = previous;
    }
    void Editor::Impl::applyMenuRequests()
    {
        const auto count = menu_requests_.size();
        for (std::size_t index{}; index < count; ++index)
        {
            const auto call = menu_requests_[index];
            if (!validMenuTarget(call))
                continue;
            lux::ui::Command command{call.command.view()};
            dispatchCommand(call, command);
            if (!command.enabled)
                continue;
            command.phase = lux::ui::ECommandPhase::EXECUTE;
            dispatchCommand(call, command);
        }
        menu_requests_.erase(menu_requests_.begin(), menu_requests_.begin() + count);
    }
    void Editor::Impl::applicationCommand(lux::ui::Command& command) noexcept
    {
        using Result = lux::ui::ECommandDispatchResult;
        const auto id = command.id.name();
        const bool execute = command.phase == lux::ui::ECommandPhase::EXECUTE;
        const bool available = !reviewing_exit_ && !exit_requested_;
        if (id == "lux.application.exit")
        {
            command.enabled = available;
            command.result = Result::DISABLED;
            if (execute && available)
            {
                requestExit();
                command.result = Result::EXECUTED;
            }
            return;
        }
        constexpr std::string_view create_prefix = "lux.window.create/";
        constexpr std::string_view show_prefix = "lux.window.show/";
        if (id.starts_with(create_prefix))
        {
            const auto type = lux::ui::PaneTypeIdView{id.substr(create_prefix.size())};
            command.enabled = available;
            command.result = Result::DISABLED;
            if (execute && available)
            {
                const auto created = context->panes().create(type);
                command.result = created ? Result::EXECUTED : Result::FAILED;
                if (!created)
                    reportMenuFailure(created.error());
            }
            return;
        }
        if (id.starts_with(show_prefix))
        {
            auto* pane = root->findPane(lux::ui::PaneIdView{id.substr(show_prefix.size())});
            command.enabled = pane && available;
            command.checked = pane && pane->visible();
            command.result = Result::DISABLED;
            if (execute && command.enabled)
            {
                context->panes().show(*pane);
                command.result = Result::EXECUTED;
            }
            return;
        }
        if (id == "lux.window.close")
        {
            auto* target = active_command_ ? active_command_->pane : menu_target_.pane;
            command.enabled = available && target;
            command.result = Result::DISABLED;
            if (execute && command.enabled)
            {
                target->requestClose();
                command.result = Result::EXECUTED;
            }
            return;
        }
        if (id == "lux.asset.save-all")
        {
            command.enabled = available && save_all_targets_.empty();
            command.result = Result::DISABLED;
            if (execute && command.enabled)
            {
                for (const auto& pane : context->panes().panes())
                {
                    ui::SaveAllRequest query;
                    if (object::sendEvent(*pane, query) && query.applicable)
                        save_all_targets_.push_back(pane->id());
                }
                save_all_started_ = false;
                command.result = Result::EXECUTED;
            }
            return;
        }
        constexpr std::string_view layout_prefix = "lux.workspace.apply/";
        if (id.starts_with(layout_prefix) || id == "lux.workspace.default")
        {
            command.enabled = available && !workspace_pending_ && !workspace_intent_;
            command.result = Result::DISABLED;
            if (execute && command.enabled)
            {
                WorkspaceRequest request;
                request.action = id == "lux.workspace.default" ? EWorkspaceAction::DEFAULT : EWorkspaceAction::APPLY;
                if (request.action == EWorkspaceAction::APPLY)
                    request.name = id.substr(layout_prefix.size());
                workspaceRequest(request);
                command.result = request.result ? Result::EXECUTED : Result::FAILED;
                if (!request.result)
                    reportMenuFailure(request.result.error());
            }
            return;
        }
        const auto entries = context->commands();
        const auto found =
            std::ranges::find_if(entries, [&](const auto& entry) { return entry.id.view() == command.id; });
        if (found == entries.end())
            return;
        command.enabled = available;
        command.result = Result::DISABLED;
        if (!available)
            return;
        // A command may replace the registry; the invocation keeps the old code and callable alive.
        if (!execute)
        {
            const auto queried = found->invoke(*context, command);
            if (!queried)
                command.enabled = false;
            return;
        }
        const auto registration = *found;
        const auto result = registration.invoke(*context, command);
        command.result = result ? Result::EXECUTED : Result::FAILED;
        if (!result)
            reportMenuFailure(result.error());
    }
    void Editor::Impl::updateSaveAll()
    {
        if (save_all_targets_.empty())
            return;
        auto* target = context->panes().find(save_all_targets_.front().view());
        if (!target)
        {
            save_all_targets_.erase(save_all_targets_.begin());
            save_all_started_ = false;
            return;
        }
        ui::SaveAllRequest request{!save_all_started_};
        const bool handled = object::sendEvent(*target, request);
        save_all_started_ = true;
        if (!handled || !request.pending)
        {
            if (!request.result)
                reportMenuFailure(request.result.error());
            save_all_targets_.erase(save_all_targets_.begin());
            save_all_started_ = false;
        }
    }
    void Editor::Impl::rebuildMenu()
    {
        const bool unchanged = !menu_dirty_ && menu_windows_ == root->windowRevision() &&
                               menu_registrations_ == context->panes().revision() &&
                               menu_commands_ == context->commandRevision();
        if (unchanged)
            return;
        using lux::ui::EKey;
        using lux::ui::MenuItem;
        std::vector<MenuItem> menus{{{}, "File"}, {{}, "Edit"}, {{}, "Window"}, {{}, "Tools"}, {{}, "Help"}};
        auto& file = menus[0].children;
        file.push_back(item("lux.asset.save", "Save", "Ctrl+S", {EKey::S, true}));
        file.push_back(item("lux.asset.save-as", "Save As", "Ctrl+Shift+S", {EKey::S, true, true}));
        file.push_back(item("lux.asset.save-all", "Save All"));
        file.push_back(item("lux.window.close", "Close Window", "Ctrl+W", {EKey::W, true}));
        file.push_back(item("lux.application.exit", "Exit"));
        auto& edit = menus[1].children;
        edit.push_back(item("lux.edit.undo", "Undo", "Ctrl+Z", {EKey::Z, true}));
        edit.push_back(item("lux.edit.redo", "Redo", "Ctrl+Y", {EKey::Y, true}));
        for (const auto& [id, label] : std::array<std::pair<std::string_view, std::string_view>, 5>{
                 {{"cut", "Cut"},
                  {"copy", "Copy"},
                  {"paste", "Paste"},
                  {"delete", "Delete"},
                  {"select-all", "Select All"}}
             })
            edit.push_back(item(std::string{"lux.edit."} + std::string(id), std::string(label)));
        auto& windows = menus[2].children;
        MenuItem create{{}, "Open Window"};
        for (const auto& entry : context->panes().registrations())
            create.children.push_back(item("lux.window.create/" + std::string(entry.type.name()), entry.name));
        windows.push_back(std::move(create));
        MenuItem layouts{{}, "Layouts"};
        layouts.children.push_back(item("lux.workspace.default", "Restore Default"));
        for (const auto& name : workspace_.names)
            layouts.children.push_back(item("lux.workspace.apply/" + name, name));
        windows.push_back(std::move(layouts));
        for (auto* pane : root->panes())
            if (pane && !pane->modal())
                windows.push_back(item("lux.window.show/" + std::string(pane->id().name()), std::string(pane->title()))
                );
        for (const auto& entry : context->commands())
        {
            if (entry.menu.empty())
                continue;
            auto* level = &menus;
            std::string_view path = entry.menu;
            while (!path.empty())
            {
                const auto slash = path.find('/');
                const auto name = path.substr(0, slash);
                auto target = std::ranges::find(*level, name, &MenuItem::label);
                if (target == level->end())
                {
                    level->push_back({{}, std::string(name)});
                    target = std::prev(level->end());
                }
                level = &target->children;
                path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
            }
            level->push_back({entry.id, entry.label, entry.shortcut_label, entry.shortcut});
        }

        if (!menu_error_.empty())
            menus.back().children.push_back({{}, menu_error_});
        root->setMenu(std::move(menus));
        menu_windows_ = root->windowRevision();
        menu_registrations_ = context->panes().revision();
        menu_commands_ = context->commandRevision();
        menu_dirty_ = false;
    }
}
