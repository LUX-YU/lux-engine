#include "ObjectQueue.hpp"
#include <atomic>
#include <cassert>
#include <fstream>
#include <functional>
#include <iostream>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/sessions/SessionOperations.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/storage/ProjectCommands.hpp>
#include <lux/engine/editor/storage/ProjectContentOpening.hpp>
#include <lux/engine/editor/storage/ProjectContentSaving.hpp>
#include <lux/engine/editor/storage/ProjectPluginSelection.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/RecentProjects.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <thread>

using namespace lux;
using namespace lux::editor;
static_assert(kSaveAsCommand.id.name() == "lux.editor.save-as");
static_assert(kExportCopyCommand.id.name() == "lux.editor.export-copy");
static_assert(kReloadCommand.scope == commands::ECommandScope::SESSION);
static_assert(kSaveAllCommand.id.name() == "lux.editor.save-all");
namespace p = lux::editor::persistence;
namespace s = lux::editor::sessions;
namespace em = lux::editor::material;
namespace es = lux::editor::scene;
namespace ef = lux::editor::flowforge;
namespace
{
    template <class T> auto take(T result)
    {
        if (!result)
        {
            if constexpr (requires { result.error().domain; })
                std::cerr << result.error().domain << '\n';
            if constexpr (std::is_same_v<typename T::error_type, EditorFailure>)
                if (const auto* cause = std::any_cast<s::SessionFactoryFailure>(&result.error().cause))
                    std::cerr << cause->domain << " code=" << static_cast<unsigned>(cause->code) << '\n';
            std::abort();
        }
        return std::move(*result);
    }
    auto id(std::string_view text)
    {
        return asset::AssetId{
            uuids::uuid_name_generator(*uuids::uuid::from_string("57279371-1b9c-40d6-b55d-a1a12065d932"))(text)
        };
    }
    std::vector<std::byte> read(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        assert(file);
        const std::string bytes{std::istreambuf_iterator<char>{file}, {}};
        const auto view = std::as_bytes(std::span{bytes});
        return {view.begin(), view.end()};
    }
    class Files final : public p::IArtifactStore
    {
    public:
        explicit Files(const std::filesystem::path& root)
            : real_(root), manifest_key_(take(real_.resolve("Project.luxproject")).key)
        {
        }
        // Only publication result delivery is injected; every successful write uses the real backend.
        std::atomic<int> fault{};
        std::function<void()> during_resolve;
        p::PersistenceResult<p::WriteTarget> resolve(std::string_view address) override
        {
            if (auto callback = std::exchange(during_resolve, {}))
                callback();
            return real_.resolve(address);
        }
        p::VPublicationOutcome publish(const p::PublicationQuery& query, std::stop_token stop) override
        {
            const bool is_manifest = query.target.key == manifest_key_;
            const auto mode = fault.load();
            const bool applies = (is_manifest && (mode == 1 || mode == 2)) || (!is_manifest && mode == 3);
            const auto selected = applies ? fault.exchange(0) : 0;
            if (selected == 1)
                return p::NotPublished{{p::EPersistenceError::IO, "injected manifest refusal"}};
            auto result = real_.publish(query, stop);
            if (selected >= 2 && std::holds_alternative<p::CommitReceipt>(result))
                return p::PublicationUnknown{{p::EPersistenceError::IO, "lost receipt"}, "published"};
            return result;
        }
        p::Reconciliation reconcile(const p::PublicationQuery& query) override
        {
            return real_.reconcile(query);
        }

    private:
        storage::FileArtifactStore real_;
        p::WriteTargetKey manifest_key_;
    };
} // namespace
int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]) /
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root / "Content");
    const auto project_file = root / "Project.luxproject";
    {
        std::ofstream output(project_file);
        output << take(encodeProjectManifest({id("project"), "Independent project saving", {}, {}}));
    }
    auto runtime = take(process::ExecutionRuntime::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}}));
    auto messages = take(object::ObjectMessageQueue::create(32));
    process::TaskScope tasks{runtime};
    asset::AssetVfs vfs;
    auto prepared_project = take(prepareProjectOpen(project_file));
    auto project =
        take(ProjectStorage::open(prepared_project, vfs, *runtime.blocking(), tasks, messages.dispatcherRef()));
    Files files{root};
    p::WriteCoordinator writes;
    p::SaveService saves{writes};
    lux::test::ObjectQueue store_messages;
    s::SessionStore store{store_messages.dispatcherRef(), 8};
    services::ServiceRegistry dependencies{store_messages.dispatcherRef()};
    assert(dependencies.publish({services::ServiceEntry::bind<ef::kFlowEnvironmentService>(object::CodeLease::builtin())
    }));
    auto scope = take(dependencies.createScope());
    s::SessionOpening opening{runtime, store, saves, dependencies, scope};
    p::SaveExecution execution{runtime, saves, writes, files};
    ProjectContentSaving saving{store, opening, saves, *project, writes, files, runtime, execution};
    auto schemas = take(simulation::ecs::ComponentSchemaSet::build({}));
    auto factories = take(s::SessionFactorySnapshot::create(
        {es::makeSceneSessionFactory(schemas), em::makeMaterialSessionFactory(), ef::makeFlowSessionFactory()}
    ));
    auto simulation = take(std::move(simulation::SimulationDescriptionBuilder{}).build());
    auto description = take(std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved());
    auto package = take(lux::scene::createScenePackage(
        id("scene"),
        "Scene",
        {},
        std::make_shared<const simulation::SimulationDescription>(std::move(simulation)),
        description
    ));
    lux::material::MaterialSource material{id("material"), "Material", {}};
    material.graph.addNode(std::make_unique<lux::material::ConstantNode>());
    lux::flowforge::FlowGraph graph;
    graph.addNodes(std::make_unique<lux::flowforge::BranchNode>());
    auto flow = take(lux::flowforge::captureFlowSource(id("flow"), "Flow", graph));
    std::array<s::SessionPreparation, 3> inputs{
        es::prepareSceneSession({std::move(package)}, {}, {}, schemas),
        em::prepareMaterialSession({std::move(material)}, {}, {}),
        ef::prepareFlowSession({std::move(flow)}, {}, {}, {})
    };
    std::array<s::SessionId, 3> sessions;
    auto turn = [&]
    {
        assert(runtime.collectCompletions());
        assert(opening.update());
        saves.adoptCompletions();
        const auto updated = saving.update();
        if (!updated)
            std::cerr << "project update: " << updated.error().domain << '\n';
        assert(updated);
        assert(execution.submitReady());
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    };
    auto until = [&](auto ready)
    {
        for (unsigned i{}; i < 10000 && !ready(); ++i)
            turn();
        assert(ready());
    };
    for (std::size_t i{}; i < inputs.size(); ++i)
    {
        const auto request =
            take(opening.create(project->catalogModel().reference({}).project_instance, std::move(inputs[i]), factories)
            );
        assert(opening.update());
        const auto result = take(opening.status(request));
        assert(result.stage == s::EOpenAssetStage::PUBLISHED);
        sessions[i] = result.session;
        assert(opening.acknowledge(request));
        const auto before = take(store.describe(result.session));
        const auto path = "Content/source-" + std::to_string(i);
        take(saving.request(before.current, p::ESaveMode::SAVE_AS, path));
        until([&] { return saving.settled(); });
        const auto after = take(store.describe(result.session));
        assert(after.current == before.current && !after.dirty && after.binding);
        assert(project->asset(after.binding->asset)->source_path == path);
        assert(!saving.reports().back().failure && saving.reports().back().result->adoption == p::EAdoption::APPLIED);
    }
    assert(take(es::SceneCodec::decode(read(root / "Content/source-0"))).source.scene);
    assert(take(em::MaterialCodec::decode(read(root / "Content/source-1"))).source.name == "Material");
    assert(take(ef::FlowCodec::decode(read(root / "Content/source-2"))).source.name == "Flow");
    const auto key = take(store.key<em::MaterialSession>(sessions[1]));
    auto& model = take(store.access<em::MaterialSession>().edit(key)).get();
    auto rename = [&](std::string name)
    {
        em::MaterialEditBatch batch{model.describe().current, "Rename", {}};
        batch.edits.emplace_back(em::MaterialRename{std::move(name)});
        assert(model.apply(std::move(batch)));
    };
    rename("Edited");
    const auto before_copy = model.describe();
    take(saving.request(before_copy.current, p::ESaveMode::EXPORT_COPY, "Content/export"));
    until([&] { return saving.settled(); });
    const auto after_copy = model.describe();
    assert(after_copy.current == before_copy.current && after_copy.binding == before_copy.binding && after_copy.dirty);
    assert(model.undo() && model.redo());

    const auto stale = model.describe().current;
    files.during_resolve = [&] { rename("Concurrent change"); };
    const auto refused = saving.request(stale, p::ESaveMode::SAVE_AS, "Content/stale");
    assert(!refused && !std::filesystem::exists(root / "Content/stale"));
    assert(model.describe().current != stale && model.describe().dirty && saving.settled());

    files.fault = 1;
    take(saving.request(model.describe().current, p::ESaveMode::SAVE_AS, "Content/uncatalogued"));
    until([&] { return saving.settled(); });
    const auto partly_saved = model.describe();
    assert(!partly_saved.dirty && partly_saved.binding && !project->asset(partly_saved.binding->asset));
    assert(saving.reports().back().failure && std::filesystem::exists(root / "Content/uncatalogued"));
    take(saving.request(partly_saved.current, p::ESaveMode::SAVE));
    until([&] { return saving.settled(); });
    assert(project->asset(partly_saved.binding->asset) && !saving.reports().back().failure);
    assert(model.undo() && model.redo() && model.describe().current == partly_saved.current);

    for (const auto fault : {2, 3})
    {
        std::cout << "publication uncertainty at " << (fault == 2 ? "manifest" : "source") << std::endl;
        rename("Unknown " + std::to_string(fault));
        files.fault = fault;
        const auto accepted = take(saving.request(model.describe().current, p::ESaveMode::SAVE));
        std::optional<p::WriteTicket> uncertain;
        until(
            [&]
            {
                const auto& report = saving.reports().back();
                const auto status = take(saves.status(accepted));
                const auto ticket = report.catalog_ticket.value_or(status.ticket);
                if (take(writes.status(ticket)).stage == p::EWriteStage::UNKNOWN)
                    uncertain = ticket;
                return uncertain.has_value();
            }
        );
        assert(!saving.settled() && !saving.acknowledge(accepted));
        assert(writes.reconcile(*uncertain, files));
        until([&] { return saving.settled(); });
        const auto& result = *saving.reports().back().result;
        assert(std::holds_alternative<p::CommitReceipt>(result.publication));
        assert(
            project->asset(model.describe().binding->asset)->source_digest ==
            take(projectFileDigest(root / "Content/uncatalogued"))
        );
    }
    rename("No view Save All");
    const auto unknown_baseline = model.describe();
    const auto published_bytes = read(root / "Content/uncatalogued");
    assert(saving.saveAll());
    until([&] { return saving.settled(); });
    // Reconciliation records a disk fact; it does not silently reinstate an abandoned source adoption.
    const auto& conflict = std::get<p::NotPublished>(saving.reports().back().result->publication);
    assert(conflict.failure.code == p::EPersistenceError::CONFLICT);
    assert(model.describe().current == unknown_baseline.current && model.describe().dirty);
    assert(
        model.describe().binding == unknown_baseline.binding && read(root / "Content/uncatalogued") == published_bytes
    );
    assert(saving.acknowledgeSaveAll());
    take(saving.request(model.describe().current, p::ESaveMode::SAVE_AS, "Content/recovered"));
    until([&] { return saving.settled(); });
    rename("Saved without a view");
    assert(saving.saveAll());
    until([&] { return saving.settled(); });
    assert(saving.saveAllEntries().size() == 3 && !model.describe().dirty);
    assert(saving.acknowledgeSaveAll());
    for (const auto session : sessions)
        assert(opening.find(session)->close(take(store.describe(session)).current));
    assert(opening.update());
    // Both ordinary opens and restoration use this public project-source admission.
    // Reopen all three persisted domains without creating Application, Root or a view.
    for (const auto path : {"Content/source-0", "Content/recovered", "Content/source-2"})
    {
        const auto& assets = project->manifest().assets;
        const auto entry = std::ranges::find(assets, path, &ProjectAssetEntry::source_path);
        assert(entry != assets.end());
        const auto source_id = entry->id;
        const auto request =
            take(openProjectContent(*project, files, opening, project->reference(source_id), factories));
        until([&] { return take(opening.status(request)).stage == s::EOpenAssetStage::PUBLISHED; });
        const auto reopened = take(opening.status(request)).session;
        const auto state = take(store.describe(reopened));
        assert(state.binding && state.binding->asset == source_id && !state.dirty);
        assert(opening.acknowledge(request));
        assert(opening.find(reopened)->close(state.current));
        assert(opening.update());
    }
    opening.requestStop();
    assert(opening.settled() && writes.size() == 0);
    {
        ProjectPluginSelection plugins{*project, runtime, writes, files, execution};
        const auto original = project->manifest().plugins;
        const std::vector<ProjectPluginEntry> desired{{"ec3.selection", 1, {}}};
        auto stale_selection = plugins.request(desired, {});
        assert(!stale_selection && stale_selection.error().code == EEditorError::STALE_REQUEST);
        assert(!plugins.status() && writes.size() == 0 && project->manifest().plugins == original);
        files.fault = 2;
        assert(plugins.request(original, desired));
        auto busy_selection = plugins.request(original, desired);
        assert(!busy_selection && busy_selection.error().code == EEditorError::BUSY);
        assert(project->manifest().plugins == original);
        until(
            [&]
            {
                assert(plugins.update());
                return plugins.status() && std::holds_alternative<EditorFailure>(*plugins.status());
            }
        );
        assert(!plugins.settled() && !plugins.acknowledge());
        assert(project->manifest().plugins == original && writes.size() == 1);
        assert(plugins.retry());
        until(
            [&]
            {
                assert(plugins.update());
                return plugins.settled();
            }
        );
        assert(std::holds_alternative<PublicationSucceeded>(*plugins.status()));
        assert(project->manifest().plugins == desired && writes.size() == 0);
        assert(plugins.acknowledge() && !plugins.status());
        stale_selection = plugins.request(original, {});
        assert(!stale_selection && stale_selection.error().code == EEditorError::STALE_REQUEST);
        bool wrong_thread{};
        std::jthread(
            [&]
            {
                const auto refused = plugins.request(desired, {});
                wrong_thread = !refused && refused.error().domain == "plugins.owner-thread";
            }
        ).join();
        assert(wrong_thread && !plugins.status() && writes.size() == 0);
        assert(plugins.request(desired, {}));
        assert(plugins.abandon());
        until(
            [&]
            {
                assert(plugins.update());
                return plugins.settled();
            }
        );
        assert(std::holds_alternative<PublicationAbandoned>(*plugins.status()));
        assert(project->manifest().plugins == desired && plugins.acknowledge() && writes.size() == 0);
    }
    {
        const auto user_root = root.parent_path() / (root.filename().string() + "-user");
        std::filesystem::create_directories(user_root);
        Files user_files{user_root};
        p::WriteCoordinator user_writes;
        p::SaveService user_saves{user_writes};
        p::SaveExecution user_execution{runtime, user_saves, user_writes, user_files};
        const auto path = user_root / "lux/editor/recent-projects.toml";
        auto settle = [&](RecentProjects& recent, auto ready)
        {
            for (unsigned i{}; i < 10000 && !ready(); ++i)
            {
                assert(runtime.collectCompletions());
                assert(recent.update());
                assert(user_execution.submitReady());
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            assert(ready());
        };
        RecentProjects recent{user_root, project_file, runtime, user_writes, user_files, user_execution};
        // Initial work has not been submitted yet; settled is a completion fact, not the refresh intent.
        assert(recent.update());
        settle(recent, [&] { return recent.settled(); });
        assert(!recent.failure() && recent.entries().size() == 1);
        assert(recent.entries().front() == project_file && recent.publication());
        assert(std::holds_alternative<p::CommitReceipt>(*recent.publication()));
        assert(std::filesystem::exists(path) && !std::filesystem::exists(root / "lux/editor/recent-projects.toml"));
        const auto saved = read(path);
        {
            std::ofstream invalid(path, std::ios::binary);
            invalid << "version = 9\nprojects = []\n";
        }
        const auto malformed = read(path);
        assert(recent.refresh() && recent.update());
        settle(recent, [&] { return recent.settled(); });
        assert(recent.failure() && recent.failure()->domain == "recent.format");
        assert(recent.entries().size() == 1 && read(path) == malformed && user_writes.size() == 0);
        {
            std::ofstream restore(path, std::ios::binary);
            restore.write(reinterpret_cast<const char*>(saved.data()), saved.size());
        }
        user_files.fault = 3;
        assert(recent.refresh() && recent.update());
        settle(
            recent,
            [&]
            {
                const auto ticket = recent.ticket();
                return ticket && take(user_writes.status(*ticket)).stage == p::EWriteStage::UNKNOWN;
            }
        );
        assert(!recent.settled() && recent.entries().size() == 1 && user_writes.size() == 1);
        assert(recent.reconcile());
        settle(recent, [&] { return recent.settled(); });
        assert(!recent.failure() && user_writes.size() == 0);
        assert(std::holds_alternative<p::CommitReceipt>(*recent.publication()));
        // Destroy with an accepted publication: the original coordinator/executor settles it.
        {
            RecentProjects closing{user_root, project_file, runtime, user_writes, user_files, user_execution};
            assert(closing.update());
            settle(closing, [&] { return closing.ticket().has_value(); });
        }
        assert(user_writes.size() == 0 && read(path) == saved);
    }
    assert(scope.release());
    (void)store_messages.collect();
    assert(dependencies.drained());
    project->requestClose();
    assert(take(project->advanceClose()));
    std::cout
        << "PASS independent three-model project save, checkpoint/history, strict source, partial publication, Unknown and Save All\n";
}
