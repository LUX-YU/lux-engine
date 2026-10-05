#include <atomic>
#include <cassert>
#include <fstream>
#include <functional>
#include <iostream>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/flowforge/PublishFlowArtifact.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/storage/ProjectContentSaving.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <thread>

using namespace lux;
using namespace lux::editor;
namespace p = lux::editor::persistence;
namespace s = lux::editor::sessions;
namespace f = lux::editor::flowforge;
namespace
{
    template <class T> auto take(T result)
    {
        if (!result)
        {
            std::abort();
        }
        return std::move(*result);
    }
    class Files final : public p::IArtifactStore
    {
    public:
        explicit Files(const std::filesystem::path& path) : file_(path) {}
        std::atomic_bool lose{};
        std::function<void()> resolving;
        p::PersistenceResult<p::WriteTarget> resolve(std::string_view path) override
        {
            if (auto callback = std::exchange(resolving, {}))
            {
                callback();
            }
            return file_.resolve(path);
        }
        p::VPublicationOutcome publish(const p::PublicationQuery& query, std::stop_token stop) override
        {
            auto result = file_.publish(query, stop);
            if (lose.exchange(false) && std::holds_alternative<p::CommitReceipt>(result))
            {
                return p::PublicationUnknown{{p::EPersistenceError::IO, "Lost real file receipt"}, "published"};
            }
            return result;
        }
        p::Reconciliation reconcile(const p::PublicationQuery& query) override
        {
            return file_.reconcile(query);
        }

    private:
        storage::FileArtifactStore file_;
    };
    class Cleanup final : public p::IArtifactSource
    {
    public:
        explicit Cleanup(std::function<void()> call) : call_(std::move(call)) {}
        ~Cleanup() override
        {
            call_();
        }
        p::PersistenceResult<p::EncodedArtifact> encode(std::stop_token) const override
        {
            std::abort(); // Rejected input must never reach its encoder.
        }

    private:
        std::function<void()> call_;
    };
} // namespace

int main(int argc, char** argv)
{
    assert(argc == 3);
    const auto root = std::filesystem::absolute(argv[1]) /
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root / "Content");
    const auto asset = asset::AssetId{*uuids::uuid::from_string("b10f86e9-488a-4567-a895-dba1b799a670")};
    const auto path = root / "Project.luxproject";
    {
        std::ofstream file(path);
        file << take(encodeProjectManifest({asset, "No UI publication", {}, {}}));
    }
    auto execution =
        take(process::ExecutionRuntime::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}}));
    auto messages = take(object::ObjectMessageQueue::create(64));
    process::TaskScope tasks{execution};
    asset::AssetVfs vfs;
    auto opened = take(prepareProjectOpen(path));
    auto project = take(ProjectStorage::open(opened, vfs, *execution.blocking(), tasks, messages.dispatcherRef()));
    Files files{root};
    p::WriteCoordinator writes;
    p::SaveService saves{writes};
    s::SessionStore authors{messages.dispatcherRef(), 8};
    services::ServiceRegistry dependencies{messages.dispatcherRef()};
    assert(dependencies.publish(
        {services::ServiceEntry::bind<f::kFlowEnvironmentService>(object::CodeLease::builtin()),
         services::ServiceEntry::bind<kProjectContentSavingService>(object::CodeLease::builtin())}
    ));
    auto scope = take(dependencies.createScope());
    s::SessionOpening opening{execution, authors, saves, dependencies, scope};
    p::SaveExecution io{execution, saves, writes, files};
    assert(scope.provide(services::ServiceNameView{"lux.editor.sessions"}, authors));
    assert(scope.provide(services::ServiceNameView{"lux.editor.sessions.opening"}, opening));
    assert(scope.provide(services::ServiceNameView{"lux.editor.persistence.saves"}, saves));
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.storage"}, *project));
    assert(scope.provide(services::ServiceNameView{"lux.editor.persistence.writes"}, writes));
    assert(
        scope.provide(services::ServiceNameView{"lux.editor.persistence.files"}, static_cast<p::IArtifactStore&>(files))
    );
    assert(scope.provide(services::ServiceNameView{"lux.process.execution"}, execution));
    assert(scope.provide(services::ServiceNameView{"lux.editor.persistence.execution"}, io));
    assert(dependencies.drained()); // Declarations did not construct the publication owner.
    auto saving_owner = take(dependencies.get<ProjectContentSaving>(scope));
    assert(take(dependencies.get<ProjectContentSaving>(scope)).get() == saving_owner.get());
    auto& saving = *saving_owner;
    auto submission = take(dependencies.get<p::IArtifactSubmission>(scope));
    assert(submission.get() == static_cast<p::IArtifactSubmission*>(saving_owner.get()));
    const auto factories = take(s::SessionFactorySnapshot::create({f::makeFlowSessionFactory()}));
    lux::flowforge::FlowGraph graph;
    auto node = graph.addNodes(std::make_unique<lux::flowforge::OnEventNode>("tick"));
    assert(graph.addExport({lux::flowforge::FlowForgeExportNodeId{1}, graph.getNode(node).node->id(), 1234}));
    auto initial = take(lux::flowforge::captureFlowSource(asset, "Frozen Flow", graph));
    auto request = take(opening.create(
        project->catalogModel().reference({}).project_instance,
        f::prepareFlowSession({std::move(initial)}, {}, {}, {}),
        factories
    ));
    assert(opening.update());
    const auto session = take(opening.status(request)).session;
    assert(session.valid() && opening.acknowledge(request));
    const auto key = take(authors.key<f::FlowSession>(session));
    auto& model = take(authors.access<f::FlowSession>().edit(key)).get();
    auto turn = [&]
    {
        assert(execution.collectCompletions());
        assert(execution.dispatchTaskEvents());
        assert(opening.update());
        saves.adoptCompletions();
        assert(saving.update());
        assert(io.submitReady());
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    };
    auto until = [&](auto ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!ready())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            turn();
        }
    };
    take(saving.request(model.describe().current, p::ESaveMode::SAVE_AS, "Content/flow.source"));
    until([&] { return saving.settled(); });
    const auto baseline = model.describe();
    const auto encoded = take(take(model.read()).encode());
    f::FlowCompilationService compiler{execution};
    auto compiling = take(compiler.start(take(model.capture()), f::FlowEnvironment{}, {}, {"EC4-missing-linker.exe"}));
    until([&] { return take(compiler.operation(compiling)).get().ready(); });
    const auto object = take(compiler.operation(compiling)).get().object();
    assert(object && take(compiler.operation(compiling)).get().retryable());
    assert(compiler.retryLink(compiling, {argv[2], 2}));
    until([&] { return take(compiler.operation(compiling)).get().ready(); });
    assert(take(compiler.operation(compiling)).get().object() == object);
    auto compiled = take(take(compiler.operation(compiling)).get().result());
    auto source = take(f::captureFlowArtifact(compiled));
    assert(compiler.acknowledge(compiling));
    compiled.reset(); // Publication owns bytes/source independently of the compiler's operation.

    bool cleaned{};
    auto cleanup = std::make_shared<Cleanup>(
        [&]
        {
            cleaned = true;
            auto nested = saving.requestArtifact(source);
            auto observed = saving.artifactReports();
            assert(!nested && nested.error().code == p::EPersistenceError::BUSY);
            assert(!observed && observed.error().code == EEditorError::BUSY);
        }
    );
    auto rejected = saving.requestArtifact({object::CodeLease::builtin(), {}, {}, std::move(cleanup)});
    assert(!rejected && rejected.error().code == p::EPersistenceError::INVALID_ARGUMENT && cleaned);

    std::vector<std::uint64_t> ids;
    for (unsigned i{}; i < 64; ++i)
    {
        ids.push_back(take(saving.requestArtifact(source)));
    }
    auto full = saving.requestArtifact(source);
    assert(!full && full.error().code == p::EPersistenceError::CAPACITY);
    f::FlowEditBatch rename{model.describe().current, "After captured intent", {}};
    rename.edits.emplace_back(f::FlowRename{"Changed"});
    assert(model.apply(std::move(rename)) && saving.update());
    for (const auto& report : take(saving.artifactReports()))
    {
        assert(report.terminal && !report.admitted && std::holds_alternative<EditorFailure>(report.status));
        assert(saving.acknowledgeArtifact(report.id));
    }
    assert(saving.settled() && take(saving.artifactReports()).empty());
    assert(model.undo() && model.describe().current == baseline.current);
    assert(take(take(model.read()).encode()) == encoded && !model.describe().dirty);

    const auto published = take(saving.requestArtifact(source));
    assert(published > ids.back());
    const auto before_busy = model.describe();
    const auto history_before_busy = take(model.historyView()).snapshot;
    bool store_was_busy{};
    auto peer_code = std::make_shared<Cleanup>([&]
    {
        auto current = authors.describe(session);
        store_was_busy = !current && current.error() == s::ESessionError::BUSY;
        assert(store_was_busy && saving.update());
        const auto retained = take(saving.artifactReports()).front();
        assert(retained.id == published && !retained.admitted && !retained.terminal);
        assert(std::holds_alternative<PublicationPending>(retained.status));
    });
    auto reservation = take(authors.reserve<f::FlowSession>(
        {"lux.editor.flowforge"}, object::CodeLease::plugin(peer_code)
    ));
    auto peer = take(f::FlowSession::create(reservation.id(), {}, {asset, "Closing peer", {}}));
    assert(authors.prepare(reservation, peer));
    const auto peer_id = take(authors.publish(reservation));
    peer_code.reset();
    auto permit = take(authors.prepareClose(take(authors.describe(peer_id)).current));
    assert(authors.close(permit));
    (void)messages.collectRetired();
    const auto after_busy = model.describe();
    assert(store_was_busy && after_busy.current == before_busy.current && after_busy.observed == before_busy.observed);
    assert(after_busy.binding == before_busy.binding && after_busy.dirty == before_busy.dirty);
    assert(after_busy.admission == before_busy.admission);
    const auto history_after_busy = take(model.historyView()).snapshot;
    assert(history_after_busy.history == history_before_busy.history);
    assert(history_after_busy.current == history_before_busy.current);
    assert(history_after_busy.revision == history_before_busy.revision);
    assert(history_after_busy.event_sequence == history_before_busy.event_sequence);
    assert(history_after_busy.entry_count == history_before_busy.entry_count);
    assert(history_after_busy.cursor == history_before_busy.cursor);
    assert(history_after_busy.charged_retained_bytes == history_before_busy.charged_retained_bytes);
    assert(history_after_busy.history_metadata_bytes == history_before_busy.history_metadata_bytes);
    assert(history_after_busy.closed == history_before_busy.closed);
    assert(take(take(model.read()).encode()) == encoded);
    assert(saving.update()); // Exact Session/current gate has accepted the frozen source.
    assert(take(saving.artifactReports()).front().admitted);
    auto premature = saving.acknowledgeArtifact(published);
    assert(!premature && premature.error().code == EEditorError::BUSY);
    assert(opening.find(session)->close(baseline.current));
    assert(opening.update() && !authors.describe(session));
    files.lose = true;
    bool protected_callback{};
    files.resolving = [&]
    {
        protected_callback = true;
        auto nested = saving.requestArtifact(source);
        auto acknowledged = saving.acknowledgeArtifact(published);
        assert(!nested && nested.error().code == p::EPersistenceError::BUSY);
        assert(!acknowledged && acknowledged.error().code == EEditorError::BUSY);
    };
    std::optional<p::WriteTicket> uncertain;
    until(
        [&]
        {
            auto report = take(saving.artifactReports()).front();
            if (report.ticket && take(writes.status(*report.ticket)).stage == p::EWriteStage::UNKNOWN)
            {
                uncertain = report.ticket;
            }
            return uncertain.has_value();
        }
    );
    assert(protected_callback && !saving.settled() && !saving.acknowledgeArtifact(published));
    assert(writes.reconcile(*uncertain, files));
    until([&] { return !take(saving.artifactReports()).front().ticket; });
    assert(saving.retryArtifact(published));
    until([&] { return saving.settled(); });
    auto report = take(saving.artifactReports()).front();
    assert(report.admitted && report.terminal && std::holds_alternative<PublicationSucceeded>(report.status));
    const auto* entry = project->asset(baseline.binding->asset);
    assert(entry && entry->compiled_source_digest == entry->source_digest);
    assert(std::filesystem::exists(root / report.path) && project->catalogAsset(entry->id));
    assert(saving.acknowledgeArtifact(published) && take(saving.artifactReports()).empty());
    assert(!saving.acknowledgeArtifact(published) && writes.size() == 0);
    submission.reset();
    saving_owner.reset();
    assert(scope.release());
    (void)messages.collectRetired();
    assert(scope.drained() && dependencies.drained());
    project->requestClose();
    assert(take(project->advanceClose()));
    std::cout << "PASS no-UI fixed Flow compile/retry, source cleanup, bounded publication, close-after-admission, "
                 "Unknown reconciliation, file/catalog results and explicit acknowledgement\n";
}
