#include <lux/engine/editor/project/DesktopSettings.hpp>
#include <lux/engine/editor/project/SettingsContent.hpp>
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include "../../../../cmake/installed-consumers/common/ControlsTestAccess.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>

using namespace lux;
using namespace lux::editor;
namespace
{
    template<class Result> auto take(Result result)
    {
        if (!result)
        {
            if constexpr (requires { result.error().domain; })
                std::fprintf(stderr, "failure: %s\n", result.error().domain.c_str());
            std::abort();
        }
        return std::move(*result);
    }
    class Page final : public ui::Pane
    {
    public:
        ui::Layout layout;
        project::SettingsContent settings;
        Page(object::ObjectDispatcherRef dispatcher, std::shared_ptr<project::SettingsContentInput> input)
            : Pane(dispatcher, ui::PaneId{"settings-test"}, ui::PaneTypeId{"test.settings"}, "Settings"),
              layout(*this, ui::ElementId{"layout"}, ui::ELayoutType::VERTICAL),
              settings(layout, ui::ElementId{"settings"}, std::move(input))
        { setContent(layout); }
    };
    ui::NumericEdit* numeric(object::LuxObject& node, std::string_view id)
    {
        if (auto* control = dynamic_cast<ui::NumericEdit*>(&node))
            if (control->id() == ui::ElementId{id})
                return control;
        for (auto* child = node.firstChild(); child; child = child->nextSibling())
            if (auto* found = numeric(*child, id))
                return found;
        return nullptr;
    }
    void changeScale(project::SettingsContent& settings, float scale)
    {
        auto* control = numeric(settings, "scale");
        assert(control);
        control->setValue(scale);
        assert(ui::ControlsTestAccess::edited(*control, ui::EditResult{true, true, true}).complete());
        assert(static_cast<const project::AppearanceSettings*>(settings.draft()->desired.data())->scale == scale);
    }
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto root_path = std::filesystem::path(argv[1]) /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto user_path = root_path / "user";
    const auto project_path = root_path / "profile";
    std::filesystem::create_directories(user_path);
    std::filesystem::create_directories(project_path);
    storage::FileArtifactStore files{root_path};
    persistence::WriteCoordinator writes;
    workspace::WorkspaceStore user{user_path, writes, files}, project{project_path, writes, files};
    workspace::WorkspaceChanges user_changes{user, writes, files}, project_changes{project, writes, files};
    auto messages = take(object::ObjectMessageQueue::create(128));
    commands::CommandRegistry commands;
    extensions::ContributionRegistry registry{messages.dispatcherRef(), commands};
    unsigned shortcut_applies{};
    bool block_apply{true};
    Page* active_page{};
    const auto publish = [&] {
        extensions::ContributionDraft draft;
        draft.reflection.push_back({contracts::CodeLease::builtin(), project::registerDesktopSettings});
        draft.settings = project::makeDesktopSettingsPages([&](const ConfigurationValue&) -> settings::SettingsResult<void> {
            if (block_apply)
                return cxx::unexpected(settings::SettingsFailure{settings::ESettingsError::BUSY, "test receiver busy"});
            if (active_page)
            {
                auto nested = active_page->settings.request(project::ESettingsAction::DEFAULTS);
                assert(!nested && nested.error().code == EEditorError::BUSY);
            }
            ++shortcut_applies;
            return {};
        });
        auto candidate = take(extensions::ContributionSnapshot::prepare(std::move(draft)));
        assert(registry.enqueue(candidate) && registry.applyPending());
    };
    publish();
    auto input = std::make_shared<project::SettingsContentInput>();
    input->pages = [&] {
        auto snapshot = registry.snapshot();
        return std::vector<settings::SettingsPage>{snapshot.settings().begin(), snapshot.settings().end()};
    };
    input->locations = std::vector<project::SettingsLocation>{{user, &user_changes, settings::ESettingsScope::USER, "settings.toml"},
        {project, &project_changes, settings::ESettingsScope::USER_PROJECT, "settings.toml"}};
    const auto initial = registry.snapshot().findSetting(settings::SettingsIdView{"lux.desktop.appearance"})->entry;
    auto defaults = take(initial->defaults());
    std::vector<std::byte> default_bytes;
    assert(defaults.encode(default_bytes));
    input->applied.push_back({initial, default_bytes});
    auto root = take(ui::Root::create(messages.dispatcherRef()));
    auto page = std::make_unique<Page>(messages.dispatcherRef(), input);
    auto mounted = take(root->prepareMount(*page));
    auto committed = take(root->commit(mounted));
    assert(committed.notifications.complete());
    ui::DrawData draw;
    assert(root->update({{900, 700}, 0.016f}, &draw));
    const auto select = [&] {
        assert(page->settings.select(settings::SettingsIdView{"lux.desktop.appearance"},
            settings::ESettingsScope::USER_PROJECT));
    };
    const auto settle = [&] {
        while (auto work = take(writes.takeReady()))
            assert(writes.complete(work->ticket, files.publish(*work)));
        assert(user_changes.update() && project_changes.update());
    };
    active_page = page.get();
    assert(page->settings.select(settings::SettingsIdView{"lux.desktop.shortcuts"},
        settings::ESettingsScope::USER_PROJECT));
    auto busy = page->settings.request(project::ESettingsAction::APPLY);
    assert(!busy && busy.error().code == EEditorError::BUSY && !page->settings.draft()->applied);
    const auto before = page->settings.draft()->based_on;
    block_apply = false;
    assert(page->settings.request(project::ESettingsAction::APPLY));
    assert(page->settings.draft()->applied && page->settings.draft()->based_on == before);
    select();
    changeScale(page->settings, 2.f);
    assert(page->settings.request(project::ESettingsAction::SAVE));
    assert(!page->settings.draft()->persisted && page->settings.draft()->applied == default_bytes);
    settle();
    assert(root->update({}, nullptr));
    assert(page->settings.draft()->persisted && page->settings.draft()->applied == default_bytes);
    auto stored = take(project.readSettings("settings.toml", settings::ESettingsScope::USER_PROJECT));
    auto value = take(settings::resolveSettings(initial, std::span{&stored, 1}));
    assert(static_cast<const project::AppearanceSettings*>(value.desired.data())->scale == 2.f);
    const auto adopted = page->settings.draft()->based_on;
    // Real external publication changes the file version. The actual control draft stays unchanged.
    auto external = stored;
    project::AppearanceSettings other{"", 3.f};
    external.values.front().bytes.clear();
    assert(initial->descriptor().configuration->codec.encode(&other, external.values.front().bytes));
    auto external_ticket = take(project_changes.saveSettings("settings.toml", external));
    settle();
    assert(project_changes.acknowledge(external_ticket));
    auto stale = page->settings.request(project::ESettingsAction::SAVE);
    assert(!stale && page->settings.draft()->based_on == adopted);
    assert(static_cast<const project::AppearanceSettings*>(page->settings.draft()->desired.data())->scale == 2.f);
    assert(page->settings.request(project::ESettingsAction::REVERT));
    assert(static_cast<const project::AppearanceSettings*>(page->settings.draft()->desired.data())->scale == 3.f);
    assert(page->settings.request(project::ESettingsAction::DEFAULTS));
    changeScale(page->settings, 1.5f);
    publish();
    auto replaced = page->settings.request(project::ESettingsAction::SAVE);
    assert(!replaced && replaced.error().code == EEditorError::STALE_REQUEST);
    assert(page->settings.request(project::ESettingsAction::REVERT));
    changeScale(page->settings, 1.75f);
    assert(page->settings.request(project::ESettingsAction::SAVE));
    // Accepted IO survives the UI owner. No page destruction cancels or acknowledges it.
    auto detached = take(root->prepareDetach(*page));
    auto removed = take(root->commit(detached));
    assert(removed.notifications.complete());
    active_page = nullptr;
    page.reset();
    assert(!project_changes.settled());
    settle();
    assert(project_changes.settled() && project_changes.publications().size() == 1);
    auto reopened = take(project.readSettings("settings.toml", settings::ESettingsScope::USER_PROJECT));
    auto new_entry = registry.snapshot().findSetting(settings::SettingsIdView{"lux.desktop.appearance"})->entry;
    auto result = take(settings::resolveSettings(new_entry, std::span{&reopened, 1}));
    const auto scale = static_cast<const project::AppearanceSettings*>(result.desired.data())->scale;
    auto scaled_root = take(ui::Root::create(messages.dispatcherRef(), {.scale = scale}));
    assert(scaled_root->scale() == 1.75f && scaled_root->fontAtlas());
    assert(shortcut_applies == 1 && writes.size() == 0);
    std::puts("PASS real settings controls, version/registration conflicts, defaults, UI retirement, file IO and Root restart scale");
}
