#include "../../test-support/ObjectQueue.hpp"
#include "Probe.hpp"
#include "Settings.hpp"
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/editor/desktop/ReviewView.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <fstream>
#include <iostream>
#include <cassert>
#include <thread>
#if defined(EC1_APP)
#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/workspace/RecoveryManifest.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>
#endif

using namespace lux;
using namespace lux::editor;
namespace
{
    template <class T> auto take(T result)
    {
        if (!result)
        {
            if constexpr (std::is_enum_v<typename T::error_type>)
                std::cerr << "code=" << int(result.error()) << '\n';
            if constexpr (requires { result.error().code; })
                std::cerr << "code=" << int(result.error().code) << '\n';
            if constexpr (requires { result.error().domain; })
                std::cerr << result.error().domain << '\n';
            if constexpr (requires { result.error().subject; })
                std::cerr << result.error().subject << '\n';
            if constexpr (requires { result.error().detail; })
                std::cerr << result.error().detail << '\n';
            std::abort();
        }
        if constexpr (!std::is_void_v<typename T::value_type>)
            return std::move(*result);
    }
    void write(const std::filesystem::path& path, std::span<const std::byte> bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        assert(file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
    }
    std::vector<std::byte> read(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        const std::string data{std::istreambuf_iterator<char>{file}, {}};
        const auto bytes = std::as_bytes(std::span{data});
        return {bytes.begin(), bytes.end()};
    }
    asset::AssetId assetId()
    {
        return asset::AssetId{*uuids::uuid::from_string("491f06e3-8618-4dfe-ae93-4c06b2b0e172")};
    }
    asset::AssetId copyId()
    {
        return asset::AssetId{*uuids::uuid::from_string("591f06e3-8618-4dfe-ae93-4c06b2b0e172")};
    }
    std::shared_ptr<const asset::SkeletonAsset> fixture(asset::AssetId id)
    {
        auto data = std::make_shared<rdesc::Skeleton>();
        data->bones = {
            {"root", -1, Eigen::Affine3f::Identity(), Eigen::Affine3f::Identity()},
            {"arm", 0, Eigen::Affine3f::Identity(), Eigen::Affine3f::Identity()}
        };
        data->bones[1].bind_local.translation().y() = 2;
        data->bones[1].inv_bind_world.translation().y() = -2;
        return take(asset::SkeletonAsset::create({id, asset::SkeletonAsset::asset_type}, data));
    }
    void verify(const rdesc::Skeleton& value, std::string_view root, float x)
    {
        assert(value.bones.size() == 2 && value.bones[0].name == root && value.bones[1].name == "arm");
        assert(value.bones[0].parent_index == -1 && value.bones[1].parent_index == 0);
        assert(
            value.bones[1].bind_local.translation().y() == 2 && value.bones[1].inv_bind_world.translation().y() == -2
        );
        assert(value.global_transform.translation().x() == x);
        // A mesh's existing bone indices still designate the same two bones; no reorder/reparent operation is offered.
        const std::array<int, 4> mesh_indices{1, 0, -1, -1};
        assert(value.bones[mesh_indices[0]].name == "arm" && value.bones[mesh_indices[1]].parent_index == -1);
    }
    void verifyFile(const std::filesystem::path& file, asset::AssetId id, std::string_view root, float x)
    {
        auto decoded = take(asset::TAssetSerDeser<asset::SkeletonAsset>::decode(
            id,
            cxx::SharedBytes<>::copyOf(read(file)),
            asset::AssetDecodeLimits{16 << 20, 16 << 20, 64}
        ));
        verify(decoded->data(), root, x);
    }
    void writeManifest(const std::filesystem::path& root, const ProjectManifest& manifest)
    {
        const auto json = take(encodeProjectManifest(manifest));
        write(root / "Project.luxproject", std::as_bytes(std::span{json}));
    }
    commands::CommandInvocation rename(sessions::SessionInfo info, std::string value, float x)
    {
        return commands::CommandInvocation{
            commands::SessionTarget{info.id, info.current},
            {lux::object::CodeLease::builtin(),
             cxx::typeToken<skeleton::Rename>(),
             std::make_shared<const skeleton::Rename>(0, std::move(value), x)}
        };
    }
    void settle(
        persistence::SaveExecution& work,
        persistence::SaveService& saves,
        process::ExecutionRuntime& runtime,
        persistence::SaveId id
    )
    {
        for (unsigned i{}; i != 10000; ++i)
        {
            take(work.submitReady());
            take(runtime.collectCompletions());
            saves.adoptCompletions();
            if (take(saves.status(id)).stage == persistence::ESaveStage::TERMINAL)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        std::abort();
    }
} // namespace
int main(int argc, char** argv)
{
    assert(argc == 4);
    const auto build = std::filesystem::absolute(argv[1]);
    const auto sdk = std::filesystem::absolute(argv[2]);
    const std::string mode{argv[3]};
    const auto root = build / "projects" / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string source_path = "Content/Characters/hero.luxskeleton";
    auto original = fixture(assetId());
    write(
        root / source_path,
        take(asset::TAssetSerDeser<asset::SkeletonAsset>::encode(*original, asset::AssetEncodeLimits{16 << 20}))
    );
    ProjectManifest manifest{assetId(), "Skeleton example"};
    manifest.assets.push_back({assetId(), "lux.skeleton", source_path, {}, {}, {}, source_path});
    writeManifest(root, manifest);
    lux::project::PluginCatalog plugin_catalog;
    take(plugin_catalog.read(build / "share/lux-engine/plugins/example.skeleton.json", build));
    const auto description = *plugin_catalog.find("example.skeleton");
    skeleton::Facts facts;
    auto runtime_plugin = take(lux::project::PluginLibrary::load(description));
    auto probe = take(lux::project::loadPluginLibrary(description, *description.editor_library));
    auto get_api = probe->get_symbol<const skeleton::ProbeApi*() noexcept>("skeleton_probe_api");
    assert(get_api);
    auto api = get_api();
    auto set_probe = api->set_facts;
    auto inspect = api->inspect;
    auto describe = api->describe;
    auto read_guard = api->read_guard;
    set_probe(&facts);
    if (mode != "APP")
    {
        auto messages = take(object::ObjectMessageQueue::create(64));
        auto runtime = take(process::ExecutionRuntime::create(
            {.cpu_concurrency = 2,
             .cpu_queue_capacity = 32,
             .timer = {32},
             .blocking = process::BlockingSchedulerConfig{1, 32}}
        ));
        process::TaskScope tasks{runtime};
        asset::AssetVfs vfs;
        auto project_data = take(prepareProjectOpen(root / "Project.luxproject"));
        auto project =
            take(ProjectStorage::open(project_data, vfs, take(runtime.blocking()), tasks, messages.dispatcherRef()));
        auto catalog = take(project->catalogModel().snapshot());
        assert(project->manifest().assets.front().sourceType() == asset::SkeletonAsset::asset_type);
        lux::test::ObjectQueue store_messages;
        sessions::SessionStore store{store_messages.dispatcherRef(), 4};
        persistence::WriteCoordinator writes;
        persistence::SaveService saves{writes};
        storage::FileArtifactStore disk{root};
        persistence::SaveExecution execution{runtime, saves, writes, disk};
        std::optional<extensions::EditorExtension> extension{
            take(extensions::EditorExtension::load(description, *runtime_plugin))
        };
        std::weak_ptr<const void> weak_code = extension->code();
        auto draft = take(extension->contributions());
        const extensions::SessionActivities capabilities{store, saves};
        auto active = take(extension->activate({&capabilities}));
        commands::CommandRegistry settings_commands;
        extensions::ContributionRegistry settings_registry{messages.dispatcherRef(), settings_commands};
        extensions::ContributionDraft settings_draft;
        settings_draft.reflection = draft.reflection;
        settings_draft.settings = draft.settings;
        auto settings_candidate = take(extensions::ContributionSnapshot::prepare(std::move(settings_draft)));
        take(settings_registry.enqueue(settings_candidate));
        take(settings_registry.applyPending());
        auto settings_snapshot = settings_registry.snapshot();
        const auto* display = settings_snapshot.findSetting(settings::SettingsIdView{"example.skeleton.display"});
        assert(display && display->create);
        workspace::WorkspaceStore preferences{root, writes, disk};
        workspace::WorkspaceChanges preferences_changes{preferences, writes, disk};
        settings::SettingsDocument user_settings;
        user_settings.values.push_back({"temporarily.missing.plugin", 19, {std::byte{42}}});
        auto initial_options = take(settings::resolveSettings(display->entry, {}));
        auto options = take(settings::makeSettingsDraft(initial_options, user_settings));
        static_cast<skeleton::DisplayOptions*>(options.desired.data())->show_indices = false;
        auto candidate_options = take(settings::prepareSettings(options, user_settings, *display->entry));
        const auto preferences_ticket = take(preferences_changes.saveSettings("user-settings.toml", candidate_options));
        assert(facts.settings_applied == 0); // Preparing/encoding/admitting a file never activates a feature.
        for (unsigned i{}; i != 10000 && !preferences_changes.settled(); ++i)
        {
            take(execution.submitReady());
            take(runtime.collectCompletions());
            take(preferences_changes.update());
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        assert(preferences_changes.settled());
        assert(std::holds_alternative<persistence::CommitReceipt>(*preferences_changes.publications()[0].result));
        take(preferences_changes.acknowledge(preferences_ticket));
        const auto reopened = take(preferences.readSettings("user-settings.toml", settings::ESettingsScope::USER));
        assert(reopened.values.front() == user_settings.values.front());
        auto effective_options = take(settings::resolveSettings(display->entry, std::array{reopened}));
        take(display->entry->apply(effective_options.desired));
        assert(facts.settings_applied == 1 && !facts.indices_applied);
        probe.reset(); // Remaining callable addresses are pinned by real extension/factory/role owners.
        auto factories = take(sessions::SessionFactorySnapshot::create(draft.sessions));
        auto provider = take(factories.selectSource("lux.skeleton", 1));
        auto input =
            take(project->captureSource(assetId(), 16 << 20, take(disk.resolve(source_path)).expected_version));
        sessions::SessionLoadJob job{
            provider,
            {std::move(input),
             assetId(),
             sessions::BoundSource{assetId(), take(disk.resolve(source_path)).key.value},
             take(disk.resolve(source_path))}
        };
        std::optional<sessions::SessionPreparation> completed;
        const auto owner = std::this_thread::get_id();
        take(tasks.submit(
            {.name = "Skeleton decode"},
            [scheduler = take(runtime.blocking()), job = std::move(job), owner](process::TaskReporter reporter
            ) mutable noexcept
            {
                return stdexec::then(
                    stdexec::schedule(scheduler),
                    [job = std::move(job), owner, stop = reporter.stopToken()]() mutable
                    {
                        assert(owner != std::this_thread::get_id());
                        return std::move(job).run(stop);
                    }
                );
            },
            [&](process::TTaskResult<sessions::SessionPreparation, sessions::SessionFactoryFailure>&& result) noexcept
            {
                assert(owner == std::this_thread::get_id());
                completed.emplace(take(std::move(result)));
            }
        ));
        // Removing the activation handle cannot unload code captured by an accepted job, result or factory.
        extension.reset();
        assert(!weak_code.expired());
        take(tasks.join());
        assert(completed);
        auto preparation = take(std::move(*completed).prepare(store, saves));
        auto installed = take(preparation.publish());
        const auto id = installed.id();
        const auto before = take(store.describe(id));
        verify(take(inspect(id)), "root", 0);
        auto missing_view = take(views::ViewFactorySnapshot::create({})).selectContent(before.kind);
        assert(!missing_view && store.size() == 1 && take(store.describe(id)).current == before.current);
        auto other = sessions::SessionFactoryEntry::create(
            lux::object::CodeLease::builtin(),
            sessions::SessionKindDescriptor{
                sessions::SessionKindIdView{"example.other"},
                "Other",
                {},
                sessions::SourceAuthoring{"lux.skeleton", 1, ".luxskeleton"}
            },
            [](const auto&, auto, auto) -> sessions::SessionFactoryResult<sessions::SessionPreparation>
            { return cxx::unexpected(sessions::SessionFactoryFailure{sessions::ESessionFactoryError::DECODE}); }
        );
        auto ambiguous =
            take(sessions::SessionFactorySnapshot::create({provider, other})).selectSource("lux.skeleton", 1);
        assert(!ambiguous && ambiguous.error().code == sessions::ESessionFactoryError::AMBIGUOUS);
        commands::CommandRegistry commands;
        auto commands_snapshot = take(commands::CommandRegistrySnapshot::create(active.commands));
        take(commands.publish(commands_snapshot));
        auto handle = take(commands.snapshot().find(commands::CommandIdView{"example.skeleton.rename"}));
        take(commands.execute(handle, rename(before, "pelvis", 3)));
        verify(take(inspect(id)), "pelvis", 3);
        take(installed.undo());
        verify(take(inspect(id)), "root", 0);
        take(installed.redo());
        verify(take(inspect(id)), "pelvis", 3);
        std::jthread foreign(
            [&]
            {
                auto rejected = inspect(id);
                assert(!rejected && rejected.error() == sessions::ESessionError::WRONG_THREAD);
            }
        );
        foreign.join();
        auto stamp = take(store.describe(id));
        auto callback = [&]
        {
            auto rejected = installed.close(stamp.current);
            assert(!rejected && rejected.error() == sessions::ESessionError::BUSY);
            auto denied = commands.execute(handle, rename(stamp, "wrong", 8));
            assert(!denied && denied.error().code == commands::ECommandError::BUSY);
        };
        take(read_guard(id, +[](void* context) { (*static_cast<decltype(callback)*>(context))(); }, &callback));
        assert(
            take(store.describe(id)).current == stamp.current && take(store.describe(id)).observed == stamp.observed
        );
        const auto save = take(saves.requestSave({id}));
        settle(execution, saves, runtime, save);
        assert(take(saves.status(save)).outcome->adoption == persistence::EAdoption::APPLIED);
        take(saves.acknowledge(save));
        verifyFile(root / source_path, assetId(), "pelvis", 3);
        const auto reload_before = take(store.describe(id));
        auto reload_target = take(disk.resolve(source_path));
        auto reload_input = take(project->captureSource(assetId(), 16 << 20, reload_target.expected_version));
        std::optional<sessions::SessionPreparation> reload_ready;
        process::TaskScope reload_tasks{runtime};
        sessions::SessionLoadJob reload_job{
            provider,
            {std::move(reload_input), assetId(), reload_before.binding, reload_target, 16 << 20, reload_before.current}
        };
        take(reload_tasks.submit(
            {.name = "Skeleton reload"},
            [scheduler = take(runtime.blocking()),
             job = std::move(reload_job)](process::TaskReporter reporter) mutable noexcept
            {
                return stdexec::then(
                    stdexec::schedule(scheduler),
                    [job = std::move(job), stop = reporter.stopToken()]() mutable { return std::move(job).run(stop); }
                );
            },
            [&](process::TTaskResult<sessions::SessionPreparation, sessions::SessionFactoryFailure>&& result) noexcept
            { reload_ready.emplace(take(std::move(result))); }
        ));
        take(reload_tasks.join());
        auto rejected_reload = [&]
        {
            auto result = std::move(*reload_ready).prepareReload(store);
            assert(!result && result.error().code == sessions::ESessionFactoryError::BUSY);
        };
        take(read_guard(
            id,
            +[](void* value) { (*static_cast<decltype(rejected_reload)*>(value))(); },
            &rejected_reload
        ));
        assert(take(store.describe(id)).current == reload_before.current);
        // BUSY retains the admitted decode result; retry needs no second decoder invocation.
        {
            auto prepared_reload = take(std::move(*reload_ready).prepareReload(store));
        }
        reload_ready.reset();
        const auto saved_history = take(installed.queryHistory());
        const std::string copied_path = "Content/Characters/copy.luxskeleton";
        const auto save_as =
            take(saves.requestSave({id, persistence::ESaveMode::SAVE_AS, take(disk.resolve(copied_path)), copyId()}));
        settle(execution, saves, runtime, save_as);
        assert(take(saves.status(save_as)).outcome->adoption == persistence::EAdoption::APPLIED);
        take(saves.acknowledge(save_as));
        assert(
            take(installed.queryHistory()).content == saved_history.content && take(installed.queryHistory()).can_undo
        );
        verifyFile(root / copied_path, copyId(), "pelvis", 3);
        auto snapshot = take(views::ViewFactorySnapshot::create(active.views));
        if (mode == "WINDOW")
        {
            auto ui_root = take(ui::Root::create(messages.dispatcherRef()));
            desktop::ViewHost host{*ui_root};
            const views::ViewContent content{{id}, id};
            const auto type = take(snapshot.selectContent(stamp.kind));
            const auto make = [&](std::string name)
            {
                return take(snapshot.prepare(
                    type,
                    {messages.dispatcherRef(),
                     ui::PaneId{name},
                     lux::object::CodeLease::builtin(),
                     cxx::typeToken<views::ContentViewInput>(),
                     std::make_shared<const views::ContentViewInput>(content, "Skeleton")}
                ));
            };
            auto first = make("first"), second = make("second");
            assert(!first.pane()->attachedRoot());
            auto first_id = take(host.adopt(first, views::ViewRestoreKey{"first"})).id;
            auto second_id = take(host.adopt(second, views::ViewRestoreKey{"second"})).id;
            assert(
                take(host.describe(first_id)).content == content && take(host.describe(second_id)).content == content
            );
            take(host.focus(first_id));
            take(host.drain());
            auto close_from_callback = [&](ui::Pane&)
            {
                take(host.close(first_id));
                assert(!host.drain());
            };
            take(host.withView(first_id, close_from_callback));
            take(host.drain());
            assert(!host.describe(first_id) && store.size() == 1);
            take(host.close(second_id));
            take(host.drain());
            assert(facts.panes_created == facts.panes_destroyed && facts.rows_prepared >= 4);
            assert(!facts.indices_displayed); // Actual skeleton row rendering consumes the same public setting.
        }
        auto final_stamp = take(store.describe(id)).current;
        take(installed.close(final_stamp));
        assert(!inspect(id) && inspect(id).error() == sessions::ESessionError::STALE_SESSION);
        take(execution.tasks().join());
        project->requestClose();
        assert(take(project->advanceClose()));
        std::cout << "PASS actual Skeleton DLL " << mode << " codec/edit/history/save-as/identity/read-gate/code-pin\n";
    }
#if defined(EC1_APP)
    else
    {
        using namespace application;
        std::filesystem::create_directories(root / "bin");
        for (const auto& entry : std::filesystem::directory_iterator{build / "bin"})
            if (entry.path().extension() == ".dll")
                std::filesystem::copy_file(entry.path(), root / "bin" / entry.path().filename());
        std::filesystem::copy_file(build / "share/lux-engine/plugins/example.skeleton.json", root / "skeleton.json");
        std::filesystem::copy_file(build / "share/lux-engine/plugins/example.witness.json", root / "witness.json");
        manifest.plugins.push_back({"example.skeleton", 1, "skeleton.json"});
        manifest.plugins.push_back({"example.witness", 1, "witness.json"});
        writeManifest(root, manifest);
        lux::project::PluginCatalog witness_catalog;
        take(witness_catalog.read(root / "witness.json", root));
        const auto witness_description = *witness_catalog.find("example.witness");
        auto witness = take(lux::project::loadPluginLibrary(witness_description, *witness_description.editor_library));
        auto observe = witness->get_symbol<void(skeleton::Facts*) noexcept>("ec1_observe");
        assert(observe);
        observe(&facts);
        auto app_description = description;
        app_description.root = root;
        // Load the actual project copy, not the SDK fixture's other DLL instance.
        set_probe(nullptr);
        probe = take(lux::project::loadPluginLibrary(app_description, *app_description.editor_library));
        get_api = probe->get_symbol<const skeleton::ProbeApi*() noexcept>("skeleton_probe_api");
        assert(get_api);
        api = get_api();
        set_probe = api->set_facts;
        inspect = api->inspect;
        describe = api->describe;
        set_probe(&facts);
        std::filesystem::create_directories(root / "user");
        auto app = take(EditorApplication::create(
            {.project_file = root / "Project.luxproject",
             .installation = sdk,
             .title = "SDK Skeleton",
             .width = 800,
             .height = 600,
             .offscreen = true,
             .user_directory = root / "user"}
        ));
        assert(facts.project && facts.host);
        auto content_views = [&]
        {
            auto all = take(facts.host->describeAll());
            std::erase_if(
                all,
                [](const auto& view) { return view.type != views::ViewTypeId{"example.skeleton.view"}; }
            );
            return all;
        };
        auto until = [&](auto predicate)
        {
            for (unsigned i{}; i != 10000; ++i)
            {
                take(app->update());
                if (predicate())
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            std::abort();
        };
        // Exact public entry used by the asset browser's double-click, through its real LuxObject connection.
        for (const auto& view : take(facts.host->describeAll()))
            if (view.type == views::ViewTypeId{"lux.editor.project"})
            {
                auto open = [&](ui::Pane& pane) {
                    take(static_cast<lux::editor::project::ProjectView&>(pane).requestOpen(
                        facts.project->reference(assetId())
                    ));
                };
                take(facts.host->withView(view.id, open));
            }
        until([&] { return content_views().size() == 1; });
        const auto first = content_views().front();
        const auto id = *first.content.primary;
        const auto second = take(app->show(id, true));
        assert(content_views().size() == 2 && take(facts.host->describe(second)).content == first.content);
        auto repeated = take(app->open(facts.project->reference(assetId())));
        until([&] { return take(app->openStatus(repeated)).content.stage == sessions::EOpenAssetStage::PUBLISHED; });
        assert(take(app->openStatus(repeated)).content.session == id && content_views().size() == 2);
        take(app->acknowledgeOpen(repeated));
        auto info = take(describe(id));
        take(app->execute(commands::CommandId{"example.skeleton.rename"}, rename(info, "hip", 4)));
        verify(take(inspect(id)), "hip", 4);
        take(app->execute(
            commands::CommandId{"lux.editor.undo"},
            commands::CommandInvocation{commands::SessionTarget{id}}
        ));
        verify(take(inspect(id)), "root", 0);
        take(app->execute(
            commands::CommandId{"lux.editor.redo"},
            commands::CommandInvocation{commands::SessionTarget{id}}
        ));
        info = take(describe(id));
        take(app->execute(
            commands::CommandId{"lux.editor.save"},
            commands::CommandInvocation{commands::SessionTarget{id, info.current}}
        ));
        until(
            [&]
            { return !take(describe(id)).dirty && facts.project->manifest().assets.front().source_digest.size() == 64; }
        );
        verifyFile(root / source_path, assetId(), "hip", 4);
        const auto history_before = take(describe(id)).current.state;
        auto answer = [&](desktop::EReviewChoice choice, std::string text = {})
        {
            bool found{};
            for (const auto& view : take(facts.host->describeAll()))
                if (view.type == views::ViewTypeId{"lux.editor.review"})
                {
                    auto submit = [&](ui::Pane& pane)
                    {
                        auto& prompt = static_cast<desktop::ReviewView&>(pane);
                        if (!text.empty())
                            take(prompt.setText(text));
                        take(prompt.answer(choice));
                        found = true;
                    };
                    take(facts.host->withView(view.id, submit));
                }
            assert(found);
        };
        take(app->execute(
            commands::CommandId{"lux.editor.save-as"},
            commands::CommandInvocation{commands::SessionTarget{id, take(describe(id)).current}}
        ));
        answer(desktop::EReviewChoice::SAVE, "Content/Characters/copied.luxskeleton");
        until([&] { return facts.project->manifest().assets.size() == 2; });
        auto saved_as = take(describe(id));
        assert(saved_as.current.state == history_before && saved_as.binding->asset != assetId() && !saved_as.dirty);
        const auto rebound_id = saved_as.binding->asset;
        verifyFile(root / "Content/Characters/copied.luxskeleton", rebound_id, "hip", 4);
        take(app->execute(commands::CommandId{"example.skeleton.rename"}, rename(saved_as, "discard", 9)));
        take(app->execute(
            commands::CommandId{"lux.editor.reload"},
            commands::CommandInvocation{commands::SessionTarget{id, take(describe(id)).current}}
        ));
        answer(desktop::EReviewChoice::DISCARD);
        until(
            [&]
            { return !take(describe(id)).dirty && take(describe(id)).current.state.history != history_before.history; }
        );
        verify(take(inspect(id)), "hip", 4);
        take(app->execute(commands::CommandId{"lux.editor.recovery.capture"}));
        until([&] { return std::filesystem::exists(root / ".lux/workspace/recovery.toml"); });
        auto recovery = take(workspace::decodeRecovery(read(root / ".lux/workspace/recovery.toml")));
        assert(recovery.entries.size() == 2 && recovery.entries[0].contents.size() == 1);
        take(app->closeView(second));
        assert(content_views().size() == 1 && describe(id));
        take(app->closeView(first.id));
        answer(desktop::EReviewChoice::CLOSE_CONTENT);
        until([&] { return !describe(id) && app->phase() == EApplicationPhase::RUNNING; });
        assert(content_views().empty());
        take(app->execute(commands::CommandId{"lux.editor.recovery.restore"}));
        until([&] { return content_views().size() == 2; });
        const auto recovered = *content_views().front().content.primary;
        assert(recovered != id && content_views().back().content.primary == recovered);
        verify(take(inspect(recovered)), "hip", 4);
        take(app->requestExit());
        until([&] { return app->phase() == EApplicationPhase::RELEASED; });
        app.reset();
        // Removing the provider selection leaves both authored files and catalog entries intact.
        auto reopened = take(prepareProjectOpen(root / "Project.luxproject"));
        auto next_manifest = reopened.manifest();
        std::erase_if(next_manifest.plugins, [](const auto& plugin) { return plugin.id == "example.skeleton"; });
        const auto material_id = asset::AssetId{*uuids::uuid::from_string("691f06e3-8618-4dfe-ae93-4c06b2b0e172")};
        const std::string material_path = "Content/Characters/other.material";
        const auto material_bytes = take(lux::material::encodeMaterialSource({material_id, "Other asset", {}}));
        write(root / material_path, std::as_bytes(std::span{material_bytes}));
        next_manifest.assets.push_back({material_id, "lux.material.source", material_path, {}, {}, {}, material_path});
        writeManifest(root, next_manifest);
        std::filesystem::create_directories(root / "missing-user");
        auto missing = take(EditorApplication::create(
            {.project_file = root / "Project.luxproject",
             .installation = sdk,
             .offscreen = true,
             .user_directory = root / "missing-user"}
        ));
        // Querying through an independent immutable catalog does not need any provider to decode its bytes.
        const auto catalog_only = take(prepareProjectOpen(root / "Project.luxproject"));
        assert(catalog_only.manifest().assets.size() == 3);
        verifyFile(root / "Content/Characters/copied.luxskeleton", rebound_id, "hip", 4);
        const auto absent = missing->open(facts.project->reference(rebound_id));
        assert(!absent && absent.error().domain == "asset.authoring");
        const auto supported = take(missing->open(facts.project->reference(material_id)));
        for (unsigned i{}; i != 10000; ++i)
        {
            take(missing->update());
            if (take(missing->openStatus(supported)).content.stage == sessions::EOpenAssetStage::PUBLISHED)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        const auto supported_status = take(missing->openStatus(supported));
        assert(supported_status.content.stage == sessions::EOpenAssetStage::PUBLISHED && supported_status.view);
        take(missing->acknowledgeOpen(supported));
        verifyFile(root / source_path, assetId(), "hip", 4);
        take(missing->requestExit());
        for (unsigned i{}; i != 10000 && missing->phase() != EApplicationPhase::RELEASED; ++i)
            take(missing->update());
        assert(missing->phase() == EApplicationPhase::RELEASED);
        observe(nullptr);
        std::cout << "PASS real installed Application: asset-browser open, shared views, fields, history, Save/SaveAs, "
                     "reload, close, recovery\n";
    }
#endif
    if (mode != "APP")
        assert(facts.unloaded == 1);
    else
        set_probe(nullptr);
}
