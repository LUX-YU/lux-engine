#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/project/DesktopSettings.hpp>
#include <lux/engine/editor/project/SettingsContent.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>

using namespace lux;
using namespace lux::editor;
namespace
{
    template <class Result> auto take(Result result)
    {
        assert(result);
        return std::move(*result);
    }
    class Page final : public ui::Pane
    {
    public:
        ui::Layout layout;
        project::SettingsContent settings;
        Page(object::ObjectDispatcherRef dispatcher, std::shared_ptr<project::SettingsContentInput> input)
            : Pane(dispatcher, ui::PaneId{"settings-navigation"}, ui::PaneTypeId{"test.settings"}, "Settings"),
              layout(*this, ui::ElementId{"layout"}, ui::ELayoutType::VERTICAL),
              settings(layout, ui::ElementId{"settings"}, std::move(input))
        {
            setContent(layout);
        }
    };
}
int main(int argc, char** argv)
{
    assert(argc == 3);
    const auto root_path = std::filesystem::path(argv[1]) /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root_path / "user");
    std::filesystem::create_directories(root_path / "project");
    storage::FileArtifactStore files{root_path};
    persistence::WriteCoordinator writes;
    workspace::WorkspaceStore user{root_path / "user", writes, files};
    workspace::WorkspaceStore project{root_path / "project", writes, files};
    workspace::WorkspaceChanges changes{user, writes, files};
    auto messages = take(object::ObjectMessageQueue::create(128));
    commands::CommandRegistry commands;
    extensions::ContributionRegistry registry{messages.dispatcherRef(), commands};
    extensions::ContributionDraft contributions;
    contributions.reflection.push_back({contracts::CodeLease::builtin(), project::registerDesktopSettings});
    auto builtin = project::makeDesktopSettingsPages({});
    unsigned applies{};
    auto descriptor = builtin.front().entry->descriptor();
    descriptor.scopes = settings::scopeBit(settings::ESettingsScope::USER);
    descriptor.apply = settings::ESettingsApply::SAFE_POINT;
    auto entry = settings::SettingsEntry::create(
        contracts::CodeLease::builtin(), descriptor,
        [&](const ConfigurationValue&) -> settings::SettingsResult<void> { ++applies; return {}; }
    );
    contributions.settings.push_back({entry, builtin.front().create});
    auto candidate = take(extensions::ContributionSnapshot::prepare(std::move(contributions)));
    assert(registry.enqueue(candidate));
    assert(registry.applyPending());
    auto input = std::make_shared<project::SettingsContentInput>();
    input->pages = [&]
    {
        auto snapshot = registry.snapshot();
        return std::vector<settings::SettingsPage>{snapshot.settings().begin(), snapshot.settings().end()};
    };
    const bool read_only = std::string_view{argv[2]} == "readonly";
    input->locations.push_back({user, read_only ? nullptr : &changes, settings::ESettingsScope::USER, "settings.toml"});
    input->locations.push_back({project, nullptr, settings::ESettingsScope::USER_PROJECT, "settings.toml"});
    auto root = take(ui::Root::create(messages.dispatcherRef()));
    Page page{messages.dispatcherRef(), input};
    auto mount = take(root->prepareMount(page));
    assert(take(root->commit(mount)).notifications.complete());
    ui::DrawData draw;
    assert(root->update({{800, 600}, 0.016f}, &draw));
    if (read_only)
    {
        assert(page.settings.select(entry->descriptor().id, settings::ESettingsScope::USER));
        const auto based_on = page.settings.draft()->based_on;
        auto saved = page.settings.request(project::ESettingsAction::SAVE);
        assert(!saved && saved.error().domain == "settings.read-only");
        std::printf("read-only rejected save: apply calls=%u, applied=%d, writes=%zu\n",
                    applies, int(page.settings.draft()->applied.has_value()), writes.size());
        std::fflush(stdout);
        assert(applies == 0 && !page.settings.draft()->applied && writes.size() == 0);
        assert(page.settings.draft()->based_on == based_on);
    }
    else
    {
        std::printf("first allowed page: draft=%d, error=%s\n", int(page.settings.draft() != nullptr),
                    page.settings.failure() ? page.settings.failure()->domain.c_str() : "none");
        std::fflush(stdout);
        assert(page.settings.draft());
        assert(page.settings.draft()->entry == entry);
        assert(page.settings.draft()->scope == settings::ESettingsScope::USER);
        assert(!page.settings.failure() && applies == 0 && writes.size() == 0);
    }
    std::puts("PASS settings navigation/read-only admission on the real UI and file owners");
}
