#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/project/RestoreWorkbench.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>

using namespace lux;
using namespace lux::editor;
namespace
{
    template <class T> auto take(T result)
    {
        if (!result)
        {
            if constexpr (requires { result.error().domain; })
            {
                std::cerr << result.error().domain << '\n';
            }
            std::abort();
        }
        return std::move(*result);
    }
    asset::AssetId assetId(std::string_view name)
    {
        return asset::AssetId{uuids::uuid_name_generator(uuids::uuid{})(name)};
    }
    // A real SDK extension window; the content below is the production MaterialSession.
    // Rendering/MaterialView qualification remains with the original Application/GPU regressions.
    struct ContentPane final : ui::Pane
    {
        views::ViewContent content;
        explicit ContentPane(const desktop::UiCreateInfo& info)
            : Pane(info.dispatcher, info.instance, ui::PaneTypeId{"lux.editor.material"}, "Recovered"),
              content(info.content)
        {}
    };
    constexpr sessions::SessionKindIdView kinds[]{sessions::SessionKindIdView{"lux.editor.material"}};
    const desktop::UiDescriptor kContentView{
        .type = views::ViewTypeIdView{"lux.editor.material"},
        .label = "Recovered material",
        .create = [](services::ServiceResolver&, const desktop::UiCreateInfo& input)
            -> desktop::UiResult<std::unique_ptr<ui::Pane>> { return std::make_unique<ContentPane>(input); },
        .content_kinds = kinds,
        .content = [](const ui::Pane& value) noexcept { return static_cast<const ContentPane&>(value).content; },
        .restore_content = true
    };
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto directory = std::filesystem::absolute(argv[1]) /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(directory);
    const auto project_id = assetId("EC4 recovery project");
    const auto material_id = assetId("EC4 recovery material");
    lux::material::MaterialSource source{material_id, "Recovery source", {}};
    source.graph.addNode(std::make_unique<lux::material::ConstantNode>());
    const auto bytes = take(lux::material::encodeMaterialSource(source));
    {
        std::ofstream output(directory / "Material.luxmaterial", std::ios::binary);
        output << bytes;
    }
    {
        ProjectManifest manifest{project_id, "Recovery", {}, {}};
        manifest.assets.push_back({material_id, "lux.material.source", "Material.luxmaterial", {}, {}, {}, {}});
        std::ofstream output(directory / "Project.luxproject", std::ios::binary);
        output << take(encodeProjectManifest(manifest));
    }
    auto runtime = take(process::ExecutionRuntime::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}}));
    auto messages = take(object::ObjectMessageQueue::create(64));
    process::TaskScope tasks{runtime};
    asset::AssetVfs vfs;
    auto input = take(prepareProjectOpen(directory / "Project.luxproject"));
    auto project = take(ProjectStorage::open(input, vfs, *runtime.blocking(), tasks, messages.dispatcherRef()));
    storage::FileArtifactStore files{directory};
    persistence::WriteCoordinator writes;
    persistence::SaveService saves{writes};
    sessions::SessionStore sessions{messages.dispatcherRef(), 8};
    desktop::EditorContext context{messages.dispatcherRef()};
    sessions::SessionOpening opening{runtime, sessions, saves, context.services(), context.scope()};
    persistence::SaveExecution execution{runtime, saves, writes, files};
    workspace::WorkspaceStore workspace{directory, writes, files};
    workspace::WorkspaceChanges changes{workspace, writes, files};
    extensions::ContributionRegistry contributions{messages.dispatcherRef(), context};
    extensions::ContributionDraft draft;
    draft.sessions.push_back(lux::editor::material::makeMaterialSessionFactory());
    draft.ui.push_back(desktop::UiEntry::bind<kContentView>(object::CodeLease::builtin()));
    auto catalog = take(extensions::ContributionSnapshot::prepare(std::move(draft)));
    assert(contributions.enqueue(catalog) && contributions.applyPending());
    auto root = take(ui::Root::create(messages.dispatcherRef()));
    lux::editor::project::RestoreWorkbench recovery{*project, files, sessions, opening, workspace, changes, contributions};
    const auto pump = [&]
    {
        assert(runtime.collectCompletions() && runtime.dispatchTaskEvents());
        assert(opening.update());
        assert(execution.submitReady());
        assert(changes.update(false));
        (void)messages.collectRetired();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    };
    const auto until = [&](auto ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
        while (!ready())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            pump();
        }
    };
    workspace::RecoveryManifest manifest;
    manifest.entries = {
        {views::ViewRestoreKey{"material-a"}, views::ViewTypeId{"lux.editor.material.v1"},
         {{"asset:" + uuids::to_string(material_id.uuid()), false}}, 0},
        {views::ViewRestoreKey{"material-b"}, views::ViewTypeId{"lux.editor.material"},
         {{"asset:" + uuids::to_string(material_id.uuid()), false}}, 0},
        {views::ViewRestoreKey{"unknown"}, views::ViewTypeId{"future.provider"}, {{"future:opaque", false}}, 0}
    };
    manifest.opaque.push_back({"future-data", 4, {std::byte{5}, std::byte{9}}});
    assert(changes.recordRecovery(manifest, "missing"));
    until([&] { return changes.settled(); });
    const auto original = take(workspace::encodeRecovery(take(workspace.readRecovery()).value));
    bool refuse = true;
    unsigned attempts{};
    auto present = [&](views::ViewContent content, const extensions::ContributionSnapshot& fixed,
                       views::ViewRestoreKey key, views::ViewTypeId type) -> EditorResult<ui::PaneHandle>
    {
        ++attempts;
        assert(type == views::ViewTypeId{"lux.editor.material"}); // Existing read-only migration stays intact.
        assert(content.sessions.size() == 1 && content.primary == content.sessions.front());
        if (refuse)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "test.presentation.busy"});
        }
        auto recursive = recovery.start();
        assert(!recursive && recursive.error().code == EEditorError::BUSY);
        auto factory = take(fixed.ui().find(type.view()));
        auto pane = take(context.ui().create(
            factory, context.scope(), {messages.dispatcherRef(), ui::PaneId{key.name()}, content, {}, key}
        ));
        auto* created = pane.get();
        assert(root->addSubPane(std::move(pane)));
        return take(root->identify(*created));
    };
    using Progress = lux::editor::project::ERestorationProgress;
    assert(recovery.start());
    std::thread wrong([&] { assert(recovery.start().error().domain == "recovery.owner-thread"); });
    wrong.join();
    auto busy_catalog = [&](const extensions::ContributionSnapshot&) -> extensions::ContributionResult<void>
    {
        assert(recovery.update(Progress::ACTIVE, present));
        assert(!recovery.items().front().opening && !recovery.items().front().result && sessions.size() == 0);
        return {};
    };
    assert(contributions.withSnapshot(busy_catalog));
    assert(recovery.update(Progress::ACTIVE, present));
    assert(recovery.items().front().opening);
    const auto first = *recovery.items().front().opening;
    until([&] { return take(opening.status(first)).stage == sessions::EOpenAssetStage::PUBLISHED; });
    assert(recovery.update(Progress::SUSPENDED, present));
    assert(recovery.items().front().opening == first && root->panes().empty() && attempts == 0);
    const auto session = take(opening.status(first)).session;
    const auto before = take(sessions.describe(session));
    until([&]
    {
        assert(recovery.update(Progress::ACTIVE, present));
        return recovery.items()[0].sources.size() == 1 && recovery.items()[1].sources.size() == 1;
    });
    assert(attempts > 0 && sessions.size() == 1 && root->panes().empty());
    assert(!recovery.items()[0].result && !recovery.items()[1].result);
    assert(take(sessions.describe(session)).current == before.current);
    refuse = false;
    assert(recovery.update(Progress::ACTIVE, present));
    assert(recovery.items()[0].result->has_value() && recovery.items()[1].result->has_value());
    assert(!recovery.items()[2].result->has_value());
    assert(root->panes().size() == 2 && sessions.size() == 1);
    assert(take(workspace::encodeRecovery(take(workspace.readRecovery()).value)) == original);
    auto windows = take(context.ui().describe(*root));
    assert(recovery.capture(windows));
    until([&] { return changes.settled(); });
    auto recaptured = take(workspace.readRecovery());
    assert(recaptured.value.opaque == manifest.opaque);
    assert(std::ranges::any_of(
        recaptured.value.entries,
        [](const auto& entry)
        {
            const bool is_unknown = entry.restore_key == views::ViewRestoreKey{"unknown"};
            const bool has_locator = !entry.contents.empty() && entry.contents.front().locator == "future:opaque";
            return is_unknown && has_locator;
        }
    ));
    // Cancelling recovery settles already accepted opening facts without constructing further UI.
    assert(recovery.start());
    assert(recovery.update(Progress::ACTIVE, present));
    until([&]
    {
        assert(recovery.update(Progress::CLOSING, present));
        return std::ranges::all_of(recovery.items(), [](const auto& item) { return item.result.has_value(); });
    });
    assert(root->panes().size() == 2 && sessions.size() == 1);
    assert(take(sessions.describe(session)).current == before.current);
    assert(!take(sessions.describe(session)).dirty);
    root.reset(); // Window ownership is independent of the author source and its save registration.
    assert(sessions.size() == 1 && take(sessions.describe(session)).binding == before.binding);
    opening.requestStop();
    assert(opening.settled());
    std::cout << "EC4 public workbench recovery: real material source/IO, fixed catalog, BUSY/suspension, "
                 "two windows/one session, unknown bytes and close settlement; no Application\n";
}
