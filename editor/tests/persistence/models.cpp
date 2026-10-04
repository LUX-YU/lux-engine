#include <lux/engine/editor/scene/SceneSaveSource.hpp>
#include <lux/engine/editor/scene/SceneCodec.hpp>
#include <lux/engine/editor/material/MaterialSaveSource.hpp>
#include <lux/engine/editor/material/MaterialCodec.hpp>
#include <lux/engine/editor/flowforge/FlowSaveSource.hpp>
#include <lux/engine/editor/flowforge/FlowCodec.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#include <psapi.h>
#endif

// Test-only counter: ordinary allocations in this executable/static libraries, not foreign DLL heaps.
std::atomic_uint64_t allocation_calls{};
void* operator new(std::size_t bytes)
{
    ++allocation_calls;
    if (auto* memory = std::malloc(bytes ? bytes : 1))
        return memory;
    std::abort();
}
void operator delete(void* memory) noexcept
{
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}
void* operator new[](std::size_t bytes)
{
    return ::operator new(bytes);
}
void operator delete[](void* memory) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}
#include <lux/engine/editor/material/MaterialPersistenceAccess.hpp>
#include <lux/engine/editor/scene/ScenePersistenceAccess.hpp>
#include <lux/engine/editor/flowforge/FlowPersistenceAccess.hpp>

namespace es = lux::editor::scene;
namespace em = lux::editor::material;
namespace ef = lux::editor::flowforge;
using namespace lux;
using namespace lux::editor;
using namespace lux::editor::persistence;
namespace
{
    template <class T> auto take(T result)
    {
        if (!result)
        {
            if constexpr (requires { result.error().code; })
                std::cerr << "unexpected " << int(result.error().code) << '\n';
            if constexpr (requires { result.error().detail; })
                std::cerr << result.error().detail << '\n';
            std::abort();
        }
        return std::move(*result);
    }
    asset::AssetId identity(std::string_view key)
    {
        return asset::AssetId{
            uuids::uuid_name_generator(*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc"))(key)
        };
    }
    std::vector<std::byte> read(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::string value{std::istreambuf_iterator<char>(file), {}};
        auto bytes = std::as_bytes(std::span(value));
        return {bytes.begin(), bytes.end()};
    }
    struct Fixture final
    {
        sessions::SessionStore store{8};
        WriteCoordinator writes;
        SaveService saves{writes};
        storage::FileArtifactStore disk;
        es::SceneSession* scene_session{};
        em::MaterialSession* material_session{};
        ef::FlowSession* flow_session{};
        sessions::SessionId scene_id, material_id, flow_id;
        std::unique_ptr<es::SceneSaveSource> scene_source;
        std::unique_ptr<em::MaterialSaveSource> material_source;
        std::unique_ptr<ef::FlowSaveSource> flow_source;
        std::vector<SaveSourceRegistration> registrations;
        std::filesystem::path root;
        explicit Fixture(std::filesystem::path path) : disk(path), root(std::move(path))
        {
            std::filesystem::create_directories(root);
            auto simulation = take(std::move(simulation::SimulationDescriptionBuilder{}).build());
            auto description = take(std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved());
            auto schema = world::worldDataSchemaId("test.unknown");
            auto package = take(lux::scene::createScenePackage(
                identity("scene"),
                "author scene",
                std::span{&schema, 1},
                std::make_shared<const simulation::SimulationDescription>(std::move(simulation)),
                description
            ));
            auto reservation =
                take(store.reserve<es::SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin()));
            scene_id = reservation.id();
            auto author = take(es::SceneSource::create(package, take(simulation::ecs::ComponentSchemaSet::build({}))));
            auto candidate = take(es::SceneSession::create(
                scene_id,
                sessions::BoundSource{identity("scene"), "scene.pak"},
                std::move(author)
            ));
            scene_session = candidate.get();
            assert(store.prepare(reservation, candidate));
            assert(store.publish(reservation));
            scene_source = std::make_unique<es::SceneSaveSource>(
                store.access<es::SceneSession>(),
                take(store.key<es::SceneSession>(scene_id)),
                take(disk.resolve("scene.pak")),
                sessions::BindingRevision{1}
            );
            registrations.push_back(take(saves.registerSource(*scene_source)));
            auto mr =
                take(store.reserve<em::MaterialSession>({"lux.editor.material"}, lux::object::CodeLease::builtin()));
            material_id = mr.id();
            lux::material::MaterialSource input{identity("material"), "material", {}};
            assert(input.graph.addNode(std::make_unique<lux::material::ConstantNode>()).valid());
            auto mat = take(em::MaterialSession::create(
                material_id,
                sessions::BoundSource{input.id, "material.luxmaterial"},
                std::move(input)
            ));
            material_session = mat.get();
            assert(store.prepare(mr, mat));
            assert(store.publish(mr));
            material_source = std::make_unique<em::MaterialSaveSource>(
                store.access<em::MaterialSession>(),
                take(store.key<em::MaterialSession>(material_id)),
                take(disk.resolve("material.luxmaterial")),
                sessions::BindingRevision{1}
            );
            registrations.push_back(take(saves.registerSource(*material_source)));
            auto fr = take(store.reserve<ef::FlowSession>({"lux.editor.flowforge"}, lux::object::CodeLease::builtin()));
            flow_id = fr.id();
            ef::FlowAuthoringSource flow_input{identity("flow"), "flow", {}};
            (void)flow_input.graph.addNodes(std::make_unique<lux::flowforge::BranchNode>());
            auto flow = take(ef::FlowSession::create(
                flow_id,
                sessions::BoundSource{flow_input.id, "flow.luxflow"},
                std::move(flow_input)
            ));
            flow_session = flow.get();
            assert(store.prepare(fr, flow));
            assert(store.publish(fr));
            flow_source = std::make_unique<ef::FlowSaveSource>(
                store.access<ef::FlowSession>(),
                take(store.key<ef::FlowSession>(flow_id)),
                take(disk.resolve("flow.luxflow")),
                sessions::BindingRevision{1}
            );
            registrations.push_back(take(saves.registerSource(*flow_source)));
            assert(store.size() == 3);
        }
        void edit(int turn)
        {
            es::SceneEditBatch s{scene_session->describe().current, "insert", {}};
            s.edits.emplace_back(es::SceneCreateObject{
                {{identity(std::to_string(turn)).uuid()},
                 {0},
                 {{simulation::ecs::componentSchemaId("test.unknown"), 1, {std::byte(turn)}}}}
            });
            assert(scene_session->apply(std::move(s)));
            em::MaterialEditBatch m{material_session->describe().current, "rename", {}};
            m.edits.emplace_back(em::MaterialRename{"material" + std::to_string(turn)});
            assert(material_session->apply(std::move(m)));
            ef::FlowEditBatch f{flow_session->describe().current, "rename", {}};
            f.edits.emplace_back(ef::FlowRename{"flow" + std::to_string(turn)});
            assert(flow_session->apply(std::move(f)));
        }
        void encodeAll()
        {
            std::vector<EncodeWork> work;
            while (auto next = take(saves.takeEncoding()))
                work.push_back(std::move(*next));
            for (auto it = work.rbegin(); it != work.rend(); ++it)
            {
                auto encoded = it->encoding.encode({});
                if (!encoded)
                    std::cerr << "encode failure save=" << it->id.value << " code=" << int(encoded.error().code)
                              << " detail=" << encoded.error().detail << '\n';
                assert(encoded);
                assert(saves.completeEncoding(it->id, std::move(encoded)));
            }
        }
        void publishAll()
        {
            while (auto next = take(writes.takeReady()))
                assert(writes.complete(next->ticket, disk.publish(*next)));
            saves.adoptCompletions();
        }
    };
    void checkFiles(Fixture& f, std::size_t objects, std::string_view suffix)
    {
        assert(take(es::SceneCodec::decode(read(f.root / "scene.pak"))).source.partitions[0]->objectCount() == objects);
        assert(
            take(em::MaterialCodec::decode(read(f.root / "material.luxmaterial"))).source.name ==
            "material" + std::string(suffix)
        );
        assert(take(ef::FlowCodec::decode(read(f.root / "flow.luxflow"))).source.name == "flow" + std::string(suffix));
    }
    void orderCases(Fixture& f)
    {
        const std::array ids{f.scene_id, f.material_id, f.flow_id};
        f.edit(1);
        std::vector<SaveId> requests;
        for (auto id : ids)
            requests.push_back(take(f.saves.requestSave({id})));
        f.edit(2);
        for (auto id : ids)
            requests.push_back(take(f.saves.requestSave({id})));
        f.encodeAll(); // All second snapshots finish first.
        f.publishAll();
        checkFiles(f, 2, "2");
        for (auto id : requests)
            assert(take(f.saves.status(id)).outcome->adoption == EAdoption::APPLIED);
        for (auto id : requests)
            assert(f.saves.acknowledge(id));
        assert(f.scene_session->undo() && f.material_session->undo() && f.flow_session->undo());
        for (auto id : ids)
            (void)take(f.saves.requestSave({id}));
        f.encodeAll();
        f.publishAll();
        checkFiles(f, 1, "1");
        assert(
            !f.scene_session->describe().dirty && !f.material_session->describe().dirty &&
            !f.flow_session->describe().dirty
        );
        std::cout << "X05-01/Q11/Q12 three models reversed encoding, FIFO actual files, Undo new intent PASS\n";
    }
    struct HookSource final : ISaveSource
    {
        ISaveSource& source;
        mutable std::function<void()> describe_hook;
        std::function<void()> capture_hook;
        std::function<void()> accept_hook;
        std::function<void(FrozenSave&)> frozen_hook;
        unsigned captures{}, accepts{};
        explicit HookSource(ISaveSource& value) : source(value) {}
        PersistenceResult<SaveSourceInfo> describe() const override
        {
            auto info = source.describe();
            if (auto hook = std::exchange(describe_hook, {}))
                hook();
            return info;
        }
        PersistenceResult<FrozenSave> captureForSave(
            const SaveSourceInfo& info,
            const SaveRequest& request,
            std::size_t allowance
        ) override
        {
            ++captures;
            auto frozen = source.captureForSave(info, request, allowance);
            if (auto hook = std::exchange(capture_hook, {}))
                hook();
            if (frozen && frozen_hook)
                frozen_hook(*frozen);
            return frozen;
        }
        EAdoption accept(SaveReceipt&& receipt) noexcept override
        {
            ++accepts;
            if (auto hook = std::exchange(accept_hook, {}))
                hook();
            return source.accept(std::move(receipt));
        }
    };
    // R2 uses the actual installed scheduler, material role and codecs, not a service shim.
    struct EncodingProbe final
    {
        std::atomic_uint calls{}, destroyed{};
        std::atomic_bool entered{};
        bool wait_for_stop{};
        std::optional<em::MaterialSnapshot> mismatched_identity;
    };
    class ProbedEncoding final : public IEncodeJob
    {
    public:
        ProbedEncoding(OwnedEncodeJob job, EncodingProbe& probe) : job_(std::move(job)), probe_(probe) {}
        ~ProbedEncoding() override
        {
            ++probe_.destroyed;
        }
        PersistenceResult<EncodedArtifact> encode(std::stop_token stop) override
        {
            ++probe_.calls;
            probe_.entered = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (probe_.wait_for_stop && !stop.stop_requested())
            {
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
            if (probe_.mismatched_identity)
                return em::MaterialCodec::encode(*probe_.mismatched_identity, identity("wrong-codec-id"), stop);
            return job_.encode(stop);
        }

    private:
        OwnedEncodeJob job_;
        EncodingProbe& probe_;
    };
    template <class Predicate> void waitFor(Predicate&& predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!predicate())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    }
    process::TaskId waitForEncoded(process::ExecutionRuntime& runtime)
    {
        process::TaskId id;
        waitFor([&] {
            std::optional<process::TaskInfo> latest;
            for (const auto& info : runtime.taskInfos())
                if (info.name == "Encode author source" && (!latest || info.submitted > latest->submitted))
                    latest = info;
            if (latest && latest->finished)
            {
                id = latest->id;
                return true;
            }
            return false;
        });
        return id;
    }
    void publishReady(SaveService& service, WriteCoordinator& writes, IArtifactStore& disk)
    {
        while (auto work = take(writes.takeReady()))
            assert(writes.complete(work->ticket, disk.publish(*work)));
        service.adoptCompletions();
    }

    class OwnedCleanupSource final : public ISaveSource
    {
    public:
        OwnedCleanupSource(std::unique_ptr<ISaveSource> source, std::function<void()> cleanup)
            : source_(std::move(source)), cleanup_(std::move(cleanup))
        {}
        ~OwnedCleanupSource() override
        {
            cleanup_();
        }
        PersistenceResult<SaveSourceInfo> describe() const override
        {
            return source_->describe();
        }
        PersistenceResult<FrozenSave> captureForSave(
            const SaveSourceInfo& info,
            const SaveRequest& request,
            std::size_t allowance
        ) override
        {
            return source_->captureForSave(info, request, allowance);
        }
        EAdoption accept(SaveReceipt&& receipt) noexcept override
        {
            return source_->accept(std::move(receipt));
        }

    private:
        std::unique_ptr<ISaveSource> source_;
        std::function<void()> cleanup_;
    };
    void r11InputCleanup(Fixture& f, std::string_view mode)
    {
        f.edit(1);
        auto& model = *f.material_session;
        const auto before = model.describe();
        const auto bytes = take(take(model.read()).encode());
        const auto history = take(model.historyView()).snapshot;
        auto runtime = take(process::ExecutionRuntime::create(
            {.cpu_concurrency = 1,
             .cpu_queue_capacity = 16,
             .timer = {16},
             .blocking = process::BlockingSchedulerConfig{1, 16}}
        ));
        EncodingProbe probe;
        HookSource registered(*f.material_source);
        f.registrations.clear();
        std::optional<SaveSourceRegistration> registration{take(f.saves.registerSource(registered))};
        SaveExecution execution{runtime, f.saves, f.writes, f.disk};
        std::optional<SaveId> accepted;
        if (mode == "completion")
        {
            registered.frozen_hook = [&](FrozenSave& frozen) {
                frozen.encoding = {
                    lux::object::CodeLease::builtin(),
                    std::make_unique<ProbedEncoding>(std::move(frozen.encoding), probe)
                };
            };
            accepted = take(f.saves.requestSave({f.material_id}));
            assert(execution.submitReady());
            (void)waitForEncoded(runtime);
            assert(take(f.saves.status(*accepted)).stage == ESaveStage::ENCODING);
        }
        unsigned destroyed{}, released{}, collected{};
        bool prepare_busy{}, request_busy{}, outer_preserved{};
        std::optional<SaveId> incorrectly_admitted;
        auto code = lux::object::CodeLease::plugin(std::shared_ptr<const void>(new int{1}, [&](const void* p) {
            assert(destroyed == 1);
            ++released;
            delete static_cast<const int*>(p);
        }));
        auto cleanup = [&] {
            assert(released == 0);
            ++destroyed;
            if (accepted)
                collected = static_cast<unsigned>(take(runtime.collectCompletions()));
            const auto ready = f.saves.canPrepareSource();
            prepare_busy = !ready && ready.error().code == EPersistenceError::BUSY;
            auto request = f.saves.requestSave({f.material_id});
            request_busy = !request && request.error().code == EPersistenceError::BUSY;
            if (request)
                incorrectly_admitted = *request;
        };
        auto source = std::make_unique<OwnedCleanupSource>(
            std::make_unique<em::MaterialSaveSource>(
                f.store.access<em::MaterialSession>(),
                take(f.store.key<em::MaterialSession>(f.material_id)),
                take(f.disk.resolve("material.luxmaterial")),
                sessions::BindingRevision{1}
            ),
            cleanup
        );
        if (mode == "busy")
        {
            registered.describe_hook = [&] {
                const auto result = f.saves.prepareSource(f.material_id, std::move(source), std::move(code));
                assert(!result && result.error().code == EPersistenceError::BUSY);
                const auto ready = f.saves.canPrepareSource();
                outer_preserved = !ready && ready.error().code == EPersistenceError::BUSY;
            };
            assert(!f.saves.registerSource(registered));
        }
        else if (mode == "success")
        {
            registration.reset();
            {
                auto prepared = take(f.saves.prepareSource(f.material_id, std::move(source), std::move(code)));
                assert(destroyed == 0 && released == 0);
                assert(f.saves.canPublish(prepared));
                auto published = f.saves.publish(std::move(prepared));
                assert(destroyed == 0 && released == 0);
                const auto saved = take(f.saves.requestSave({f.material_id}));
                f.encodeAll();
                f.publishAll();
                assert(take(f.saves.status(saved)).outcome->adoption == EAdoption::APPLIED);
                assert(f.saves.acknowledge(saved) && !model.describe().dirty);
                // Successful handoff ends the prepare dispatch; ordinary owner destruction isn't a reject callback.
            }
            assert(destroyed == 1 && released == 1 && f.saves.canPrepareSource());
            std::cout << "R11-02 success transfers the last code/source unit, destructor precedes code release PASS\n";
            return;
        }
        else
        {
            const bool invalid = mode == "invalid";
            const auto result = f.saves.prepareSource(
                invalid ? sessions::SessionId{} : f.material_id,
                std::move(source),
                std::move(code)
            );
            assert(
                !result &&
                result.error().code == (invalid ? EPersistenceError::INVALID_ARGUMENT : EPersistenceError::BUSY)
            );
        }
        const auto after = model.describe();
        const auto h = take(model.historyView()).snapshot;
        const bool unchanged =
            bytes == take(take(model.read()).encode()) && before.id == after.id && before.kind == after.kind &&
            before.binding == after.binding && before.current == after.current && before.observed == after.observed &&
            before.dirty == after.dirty && before.admission == after.admission && h.history == history.history &&
            h.current == history.current && h.revision == history.revision &&
            h.event_sequence == history.event_sequence && h.entry_count == history.entry_count &&
            h.cursor == history.cursor && h.charged_retained_bytes == history.charged_retained_bytes &&
            h.history_metadata_bytes == history.history_metadata_bytes && h.closed == history.closed;
        const bool completion_received = !accepted || (collected == 1 && probe.calls == 1 &&
                                                       take(f.saves.status(*accepted)).stage == ESaveStage::READY);
        std::cout << "R11 input " << mode << " destroyed=" << destroyed << " code_released=" << released
                  << " prepare_BUSY=" << prepare_busy << " request_BUSY=" << request_busy
                  << " full_author_history_binding_unchanged=" << unchanged
                  << " completion_received=" << completion_received << " outer_preserved=" << outer_preserved
                  << std::endl;
        // Keep the original runtime failure evidence observable before qualification assertions.
        assert(destroyed == 1 && released == 1 && unchanged && completion_received);
        assert(prepare_busy && request_busy && !incorrectly_admitted);
        assert(mode != "busy" || outer_preserved);
        assert(f.saves.canPrepareSource());
        if (accepted)
        {
            publishReady(f.saves, f.writes, f.disk);
            assert(take(f.saves.status(*accepted)).outcome->adoption == EAdoption::APPLIED);
            assert(f.saves.acknowledge(*accepted));
            assert(probe.calls == 1 && probe.destroyed == 1 && execution.tasks().join());
        }
        const auto resumed = take(f.saves.requestSave({f.material_id}));
        f.encodeAll();
        f.publishAll();
        assert(take(f.saves.status(resumed)).outcome->adoption == EAdoption::APPLIED);
        assert(f.saves.acknowledge(resumed));
        assert(
            !model.describe().dirty && model.undo() && model.describe().dirty && model.redo() && !model.describe().dirty
        );
    }
    void r2AcceptCase(Fixture& f, std::string_view mode)
    {
        f.registrations.clear();
        HookSource source(*f.material_source);
        auto registration = take(f.saves.registerSource(source));
        auto runtime = take(process::ExecutionRuntime::create(
            {.cpu_concurrency = 2,
             .cpu_queue_capacity = 16,
             .timer = {16},
             .blocking = process::BlockingSchedulerConfig{2, 16}}
        ));
        EncodingProbe probe;
        persistence::SaveExecution execution(runtime, f.saves, f.writes, f.disk);
        f.edit(1);
        const auto first = take(f.saves.requestSave({f.material_id}));
        f.encodeAll();
        while (auto work = take(f.writes.takeReady()))
            assert(f.writes.complete(work->ticket, f.disk.publish(*work)));
        f.edit(2);
        const auto frozen_content = f.material_session->describe().current;
        if (mode == "error")
            probe.mismatched_identity = take(f.material_session->capture());
        probe.wait_for_stop = mode == "cancel" || mode == "drain";
        source.frozen_hook = [&](FrozenSave& frozen) {
            frozen.encoding = {
                lux::object::CodeLease::builtin(),
                std::make_unique<ProbedEncoding>(std::move(frozen.encoding), probe)
            };
        };
        const auto second = take(f.saves.requestSave({f.material_id}));
        source.frozen_hook = {};
        assert(execution.submitReady());
        waitFor([&] { return probe.entered.load(); });
        if (mode == "cancel")
        {
            assert(take(f.saves.requestCancel(second)) == ECancelResult::REQUESTED);
            const auto tasks = runtime.taskInfos();
            assert(tasks.size() == 1 && runtime.requestStop(tasks.front().id));
        }
        if (mode == "drain")
            execution.tasks().requestStop();
        const auto task = waitForEncoded(runtime);
        assert(take(f.saves.status(second)).stage == ESaveStage::ENCODING);
        f.edit(3);
        const auto third = take(f.saves.requestSave({f.material_id}));
        f.encodeAll(); // W3 READY while W2's result still belongs to TaskScope.
        assert(!take(f.writes.takeReady()));
        std::size_t collected{};
        source.accept_hook = [&] {
            collected = take(runtime.collectCompletions());
            assert(collected == 1 && probe.destroyed == 1);
            assert(source.accepts == 1); // Absorption cannot dispatch another role.
            f.saves.adoptCompletions();
            auto acknowledged = f.saves.acknowledge(first);
            auto requested = f.saves.requestSave({f.material_id});
            auto cancel = f.saves.requestCancel(second);
            assert(!acknowledged && acknowledged.error().code == EPersistenceError::BUSY);
            assert(!requested && requested.error().code == EPersistenceError::BUSY);
            assert(!cancel && cancel.error().code == EPersistenceError::BUSY);
            assert(source.accepts == 1);
        };
        f.saves.adoptCompletions();
        const auto after = take(f.saves.status(second));
        assert(task);
        std::cout << "R2 real runtime collected=" << collected << " encoding_calls=" << probe.calls
                  << " stuck_encoding=" << (after.stage == ESaveStage::ENCODING) << '\n';
        if (after.stage == ESaveStage::ENCODING)
        {
            assert(f.saves.requestCancel(second));
            const auto cancelled = take(f.saves.status(second));
            const auto pending = take(f.writes.status(after.ticket));
            const auto follower = take(f.writes.status(take(f.saves.status(third)).ticket));
            const auto disk = take(em::MaterialCodec::decode(read(f.root / "material.luxmaterial")));
            std::cout << "FAIL before fix: cancel_requested=1 still_encoding="
                      << (cancelled.stage == ESaveStage::ENCODING)
                      << " reserved=" << (pending.stage == EWriteStage::RESERVED)
                      << " follower_ready=" << (follower.stage == EWriteStage::READY)
                      << " follower_blocked=" << !take(f.writes.takeReady()) << " final_disk=" << disk.source.name
                      << " root=" << f.root << std::endl;
            std::_Exit(20); // Preserve the defect, not the later destructor terminate.
        }
        const bool has_error = mode == "error" || mode == "cancel" || mode == "drain";
        if (has_error)
        {
            const auto outcome = take(f.writes.status(after.ticket)).outcome;
            assert(outcome && std::holds_alternative<NotPublished>(*outcome));
            const auto expected = mode == "error" ? EPersistenceError::INVALID_ARGUMENT : EPersistenceError::CANCELLED;
            assert(std::get<NotPublished>(*outcome).failure.code == expected);
        }
        else
        {
            auto publication = take(f.writes.takeReady());
            assert(publication && publication->ticket == after.ticket);
            assert(f.writes.complete(publication->ticket, f.disk.publish(*publication)));
            const auto disk = take(em::MaterialCodec::decode(read(f.root / "material.luxmaterial")));
            assert(disk.source.name == "material2"); // Frozen W2, not live W3.
            f.saves.adoptCompletions();
            assert(f.material_session->undo());
            assert(f.material_session->describe().current == frozen_content);
            assert(!f.material_session->describe().dirty && f.material_session->redo());
        }
        publishReady(f.saves, f.writes, f.disk);
        assert(take(em::MaterialCodec::decode(read(f.root / "material.luxmaterial"))).source.name == "material3");
        assert(!f.material_session->describe().dirty && probe.calls == 1 && probe.destroyed == 1);
        auto duplicate =
            f.saves.completeEncoding(second, lux::cxx::unexpected(PersistenceFailure{EPersistenceError::ENCODE}));
        assert(!duplicate && duplicate.error().code == EPersistenceError::BUSY);
        for (const auto id : {first, second, third})
        {
            assert(take(f.saves.status(id)).stage == ESaveStage::TERMINAL);
            assert(f.saves.acknowledge(id));
        }
        auto stale =
            f.saves.completeEncoding(second, lux::cxx::unexpected(PersistenceFailure{EPersistenceError::ENCODE}));
        assert(!stale && stale.error().code == EPersistenceError::UNKNOWN_ID);
        assert(execution.tasks().join());
        std::cout << "R05-R2-01/02/04 real Material/SaveExecution first collect in accept " << mode
                  << ": exact bytes, baseline, FIFO follower, guarded acknowledgement, once-only cleanup PASS\n";
    }

    void r2AdmissionCase(Fixture& f, bool capture, std::string_view action)
    {
        HookSource source(*f.material_source);
        const auto info = take(source.describe());
        const auto bytes = take(source.captureForSave(info, {f.material_id}, SIZE_MAX)).retained_bytes;
        SaveService service(f.writes, {.max_active_saves = 3, .snapshot_bytes = 2 * bytes, .terminal_records = 3});
        std::optional<SaveSourceRegistration> registration{take(service.registerSource(source))};
        auto runtime = take(process::ExecutionRuntime::create(
            {.cpu_concurrency = 2,
             .cpu_queue_capacity = 16,
             .timer = {16},
             .blocking = process::BlockingSchedulerConfig{2, 16}}
        ));
        EncodingProbe probe;
        persistence::SaveExecution execution(runtime, service, f.writes, f.disk);
        for (unsigned iteration{}; iteration != 12; ++iteration)
        {
            SaveRequest request{
                f.material_id,
                ESaveMode::EXPORT_COPY,
                take(f.disk.resolve((f.root / ("copy-" + std::to_string(iteration))).string())),
                identity("copy")
            };
            source.frozen_hook = [&](FrozenSave& frozen) {
                frozen.encoding = {
                    lux::object::CodeLease::builtin(),
                    std::make_unique<ProbedEncoding>(std::move(frozen.encoding), probe)
                };
            };
            const auto first = take(service.requestSave(request));
            source.frozen_hook = {};
            assert(execution.submitReady());
            assert(waitForEncoded(runtime));
            auto hook = [&] {
                assert(take(runtime.collectCompletions()) == 1);
                assert(take(service.status(first)).stage == ESaveStage::READY);
                assert(source.accepts == 0);
                auto blocked = service.takeEncoding();
                assert(!blocked && blocked.error().code == EPersistenceError::BUSY);
                if (action == "throw")
                    throw std::runtime_error("controlled role callback failure after first collection");
                if (action == "revoke")
                    registration.reset();
            };
            if (capture)
                source.capture_hook = hook;
            else
                source.describe_hook = hook;
            const auto second = service.requestSave(request);
            if (action != "success")
            {
                const auto expected =
                    action == "revoke" || !capture ? EPersistenceError::STALE_SOURCE : EPersistenceError::ENCODE;
                assert(!second && second.error().code == expected);
            }
            else
                assert(second);
            publishReady(service, f.writes, f.disk);
            assert(take(service.status(first)).stage == ESaveStage::TERMINAL);
            assert(service.acknowledge(first));
            if (second)
            {
                auto work = take(service.takeEncoding());
                assert(work && work->id == *second && service.completeEncoding(work->id, work->encoding.encode({})));
                publishReady(service, f.writes, f.disk);
                assert(service.acknowledge(*second));
            }
            if (!registration)
                registration.emplace(take(service.registerSource(source)));
            // Exactly two snapshots fit again. This detects leaked allowance, underflow and double release.
            request.destination = take(f.disk.resolve((f.root / ("capacity-" + std::to_string(iteration))).string()));
            const auto a = take(service.requestSave(request));
            const auto b = take(service.requestSave(request));
            auto full = service.requestSave(request);
            assert(!full && full.error().code == EPersistenceError::CAPACITY);
            while (auto work = take(service.takeEncoding()))
                assert(service.completeEncoding(work->id, work->encoding.encode({})));
            publishReady(service, f.writes, f.disk);
            assert(std::holds_alternative<CommitReceipt>(take(service.status(a)).outcome->publication));
            assert(std::holds_alternative<CommitReceipt>(take(service.status(b)).outcome->publication));
            assert(service.acknowledge(a) && service.acknowledge(b));
            assert(!take(service.takeEncoding()) && !take(f.writes.takeReady()));
            assert(probe.calls == iteration + 1 && probe.destroyed == iteration + 1);
        }
        execution.tasks().requestStop();
        assert(execution.tasks().join());
        std::cout << "R05-R2-03/05 " << (capture ? "capture" : "describe") << ' ' << action
                  << " 12 bounded cycles: completed input, capture rollback, exact capacity, jobs retired PASS\n";
    }

    void r2ExecutionFailure(Fixture& f)
    {
        // The existing runtime rejects task admission while its sole task slot is occupied.
        // SaveExecution must settle the already admitted save even though no encoder starts.
        auto runtime = take(process::ExecutionRuntime::create(
            {.cpu_concurrency = 1,
             .cpu_queue_capacity = 2,
             .task_capacity = 1,
             .timer = {2},
             .blocking = process::BlockingSchedulerConfig{1, 2}}
        ));
        std::atomic_bool entered{}, released{};
        auto occupying = take(runtime.submit(
            {},
            [&, cpu = runtime.cpu()](process::TaskReporter) noexcept {
                return stdexec::then(
                    stdexec::schedule(cpu),
                    [&]() -> lux::cxx::expected<void, process::EExecutionError> {
                        entered = true;
                        waitFor([&] { return released.load(); });
                        return {};
                    }
                );
            },
            [](process::TTaskResult<void, process::EExecutionError>&&) noexcept {}
        ));
        waitFor([&] { return entered.load(); });
        persistence::SaveExecution execution(runtime, f.saves, f.writes, f.disk);
        const auto first = take(f.saves.requestSave({f.material_id}));
        assert(execution.submitReady());
        const auto state = take(f.saves.status(first));
        assert(state.stage == ESaveStage::READY);
        const auto outcome = take(f.writes.status(state.ticket)).outcome;
        assert(outcome && std::get<NotPublished>(*outcome).failure.code == EPersistenceError::EXECUTION);
        f.saves.adoptCompletions();
        assert(f.saves.acknowledge(first) && f.writes.size() == 0);
        released = true;
        waitFor([&] { return runtime.taskInfo(occupying.id())->finished.has_value(); });
        assert(runtime.collectCompletions() && runtime.dispatchTaskEvents());
        assert(execution.tasks().join());
        std::cout
            << "R05-R2-02/05 actual Runtime task-capacity rejection settles EXECUTION without encoder retry PASS\n";
    }

    void assertSameSession(const sessions::SessionInfo& before, const sessions::SessionInfo& after)
    {
        assert(before.current == after.current && before.observed == after.observed);
        assert(before.binding == after.binding && before.dirty == after.dirty);
        assert(before.admission == after.admission);
    }
    void publishWithoutAdoption(Fixture& f)
    {
        while (auto next = take(f.writes.takeReady()))
            assert(f.writes.complete(next->ticket, f.disk.publish(*next)));
    }
    void revokeCase(Fixture& f)
    {
        f.registrations.clear();
        f.edit(1);
        HookSource source(*f.material_source);
        std::optional<SaveSourceRegistration> registration(take(f.saves.registerSource(source)));
        const auto before = f.material_session->describe();
        const auto content = take(em::MaterialCodec::encode(take(f.material_session->capture()), identity("material")));
        source.describe_hook = [&] {
            registration.reset();
            std::cout << "R05-01 describe revoked; real source/model still alive\n" << std::flush;
        };
        auto rejected = f.saves.requestSave({f.material_id});
        assert(!rejected && rejected.error().code == EPersistenceError::STALE_SOURCE);
        assert(source.captures == 0 && f.writes.size() == 0);
        assertSameSession(before, f.material_session->describe());
        assert(std::ranges::equal(
            take(em::MaterialCodec::encode(take(f.material_session->capture()), identity("material"))).bytes.view(),
            content.bytes.view()
        ));
        assert(!std::filesystem::exists(f.root / "material.luxmaterial"));
        registration.emplace(take(f.saves.registerSource(source)));
        const auto id = take(f.saves.requestSave({f.material_id}));
        f.encodeAll();
        f.publishAll();
        assert(take(f.saves.status(id)).outcome->adoption == EAdoption::APPLIED);
        assert(!f.material_session->describe().dirty && f.saves.acknowledge(id));
        assert(f.writes.size() == 0);
        std::cout << "R05-01 real Material revoked describe, unchanged model and restored admission PASS\n";
    }
    void recursiveCase(Fixture& f)
    {
        f.registrations.clear();
        f.edit(1);
        HookSource source(*f.material_source);
        auto registration = take(f.saves.registerSource(source));
        const auto id = take(f.saves.requestSave({f.material_id}));
        f.encodeAll();
        publishWithoutAdoption(f);
        source.accept_hook = [&] {
            assert(take(f.saves.status(id)).outcome);
            f.saves.adoptCompletions();
            auto acknowledged = f.saves.acknowledge(id);
            std::cout << "R05-02 accepts=" << source.accepts << " nested_ack=" << bool(acknowledged)
                      << " record_alive=" << bool(f.saves.status(id)) << '\n'
                      << std::flush;
            assert(!acknowledged);
            assert(source.accepts == 1 && f.saves.status(id));
        };
        f.saves.adoptCompletions();
        const auto status = take(f.saves.status(id));
        assert(status.stage == ESaveStage::TERMINAL && status.outcome->adoption == EAdoption::APPLIED);
        assert(std::holds_alternative<CommitReceipt>(status.outcome->publication));
        assert(!f.material_session->describe().dirty && source.accepts == 1);
        assert(take(em::MaterialCodec::decode(read(f.root / "material.luxmaterial"))).source.name == "material1");
        assert(f.saves.acknowledge(id) && f.writes.size() == 0);
        std::cout << "R05-02 real accept executes once, deferred recursion, execution pin and later ack PASS\n";
    }
    void acknowledgedChainCase(Fixture& f, bool acknowledge)
    {
        const std::array sessions{f.scene_id, f.material_id, f.flow_id};
        std::vector<SaveId> ids;
        for (int turn{1}; turn <= 3; ++turn)
        {
            f.edit(turn);
            for (auto session : sessions)
                ids.push_back(take(f.saves.requestSave({session})));
            f.encodeAll();
            publishWithoutAdoption(f);
            if (turn == 1)
                f.saves.adoptCompletions();
        }
        if (acknowledge)
            for (std::size_t i{}; i < 3; ++i)
            {
                assert(f.saves.acknowledge(ids[i]));
                assert(!f.saves.status(ids[i]));
            }
        assert(f.writes.size() == (acknowledge ? 6 : 9));
        f.edit(4);
        for (auto session : sessions)
            ids.push_back(take(f.saves.requestSave({session})));
        f.encodeAll();
        publishWithoutAdoption(f);
        unsigned published{};
        for (std::size_t i{9}; i < 12; ++i)
        {
            auto status = take(f.writes.status(take(f.saves.status(ids[i])).ticket));
            const bool success = status.outcome && std::holds_alternative<CommitReceipt>(*status.outcome);
            published += success;
            std::cout << "R05-04 model=" << (i - 9) << " ack_w1=" << acknowledge << " w4_published=" << success << '\n'
                      << std::flush;
        }
        std::cout << "R05-04 material_file="
                  << take(em::MaterialCodec::decode(read(f.root / "material.luxmaterial"))).source.name
                  << " scene_objects="
                  << take(es::SceneCodec::decode(read(f.root / "scene.pak"))).source.partitions[0]->objectCount()
                  << " flow_file=" << take(ef::FlowCodec::decode(read(f.root / "flow.luxflow"))).source.name << '\n'
                  << std::flush;
        assert(published == 3);
        checkFiles(f, 4, "4");
        f.saves.adoptCompletions();
        for (auto session : sessions)
            assert(!take(f.store.describe(session)).dirty);
        for (std::size_t i{acknowledge ? 3u : 0u}; i < ids.size(); ++i)
        {
            assert(take(f.saves.status(ids[i])).outcome->adoption == EAdoption::APPLIED);
            assert(f.saves.acknowledge(ids[i]));
        }
        assert(f.writes.size() == 0);
        std::cout << "R05-04 all three actual models W1 ack, delayed W2/W3 adoption, real W4 publication PASS\n";
    }
    struct HookRebind final : IPreparedRebind
    {
        std::unique_ptr<IPreparedRebind> inner;
        std::function<void()> on_apply, on_destroy;
        ~HookRebind() override
        {
            if (on_destroy)
                on_destroy();
        }
        EAdoption apply(SaveReceipt&& receipt) noexcept override
        {
            if (on_apply)
                on_apply();
            return inner->apply(std::move(receipt));
        }
    };
    void callbackCases(Fixture& f)
    {
        f.registrations.clear();
        f.edit(1);
        SaveService service(f.writes, {1, 1024 * 1024, 1});
        HookSource source(*f.material_source), replacement(*f.material_source);
        std::optional<SaveSourceRegistration> registration(take(service.registerSource(source)));
        const auto before = f.material_session->describe();
        const auto content = take(em::MaterialCodec::encode(take(f.material_session->capture()), identity("material")));
        auto unchanged = [&] {
            assertSameSession(before, f.material_session->describe());
            assert(std::ranges::equal(
                take(em::MaterialCodec::encode(take(f.material_session->capture()), identity("material"))).bytes.view(),
                content.bytes.view()
            ));
            assert(f.writes.size() == 0);
        };
        auto rejectNested = [&] {
            for (int i{}; i < 3; ++i)
            {
                auto nested = service.requestSave({f.material_id});
                assert(!nested && nested.error().code == EPersistenceError::BUSY);
            }
            auto alternate = service.registerSource(replacement);
            assert(!alternate && alternate.error().code == EPersistenceError::BUSY);
            assert(!service.takeEncoding());
            service.adoptCompletions();
        };
        // Failure, exception and revoked captures must release the full temporary admission charge.
        for (bool during_capture : {false, true})
        {
            auto& hook = during_capture ? source.capture_hook : source.describe_hook;
            hook = [&] {
                rejectNested();
                throw std::runtime_error("foreign role callback");
            };
            auto failure = service.requestSave({f.material_id});
            assert(
                !failure &&
                failure.error().code == (during_capture ? EPersistenceError::ENCODE : EPersistenceError::STALE_SOURCE)
            );
            unchanged();
            hook = [&] {
                registration.reset();
                rejectNested();
            };
            auto revoked = service.requestSave({f.material_id});
            assert(!revoked && revoked.error().code == EPersistenceError::STALE_SOURCE);
            assert(replacement.captures == 0);
            unchanged();
            registration.emplace(take(service.registerSource(source)));
        }
        source.describe_hook = rejectNested;
        source.capture_hook = rejectNested;
        const auto saved = take(service.requestSave({f.material_id}));
        auto full = service.requestSave({f.material_id});
        assert(!full && full.error().code == EPersistenceError::CAPACITY);
        auto work = take(service.takeEncoding());
        assert(work && service.completeEncoding(saved, work->encoding.encode({})));
        publishWithoutAdoption(f);
        source.accept_hook = [&] {
            registration.reset();
            rejectNested();
            auto status = take(service.status(saved));
            assert(status.stage == ESaveStage::AWAITING_ADOPTION);
            assert(std::holds_alternative<CommitReceipt>(status.outcome->publication));
            auto ack = service.acknowledge(saved);
            assert(!ack && ack.error().code == EPersistenceError::BUSY);
        };
        service.adoptCompletions();
        assert(source.accepts == 1 && take(service.status(saved)).outcome->adoption == EAdoption::APPLIED);
        assert(!f.material_session->describe().dirty);
        assert(service.acknowledge(saved) && f.writes.size() == 0);
        // A subsequent role can register, but it never completes the revoked role's request.
        registration.emplace(take(service.registerSource(replacement)));
        SaveId rebind_id;
        unsigned applied{}, destroyed{};
        replacement.frozen_hook = [&](FrozenSave& frozen) {
            assert(frozen.rebind);
            auto decorated = std::make_unique<HookRebind>();
            decorated->inner = std::move(frozen.rebind);
            auto protectedCall = [&] {
                service.adoptCompletions();
                auto ack = service.acknowledge(rebind_id);
                assert(!ack && ack.error().code == EPersistenceError::BUSY);
                assert(service.status(rebind_id));
            };
            decorated->on_apply = [&, protectedCall] {
                ++applied;
                protectedCall();
            };
            decorated->on_destroy = [&, protectedCall] {
                ++destroyed;
                protectedCall();
            };
            frozen.rebind = std::move(decorated);
        };
        const auto history = f.material_session->describe().current;
        rebind_id = take(service.requestSave(
            {f.material_id, ESaveMode::SAVE_AS, take(f.disk.resolve("callback-copy")), identity("callback-copy")}
        ));
        work = take(service.takeEncoding());
        assert(work && service.completeEncoding(rebind_id, work->encoding.encode({})));
        publishWithoutAdoption(f);
        service.adoptCompletions();
        assert(applied == 1 && destroyed == 1);
        assert(take(service.status(rebind_id)).outcome->adoption == EAdoption::APPLIED);
        assert(f.material_session->describe().current == history && !f.material_session->describe().dirty);
        assert(service.acknowledge(rebind_id));
        assert(f.writes.size() == 0);
        std::cout << "R05-03 real role revoke/replace, foreign exceptions, nested admission bounds, rebind+cleanup "
                     "guards PASS\n";
    }
    void delayedReadCase(Fixture& f)
    {
        f.edit(1);
        const auto first = take(f.saves.requestSave({f.material_id}));
        f.encodeAll();
        f.publishAll();
        const auto first_content = f.material_session->describe().current;
        f.edit(2);
        const auto second = take(f.saves.requestSave({f.material_id}));
        f.encodeAll();
        publishWithoutAdoption(f);
        auto view = take(f.material_session->read());
        assert(view.withRead([&](const auto&) -> em::MaterialEditResult<void> {
            f.saves.adoptCompletions();
            const auto status = take(f.saves.status(second));
            assert(status.stage == ESaveStage::AWAITING_ADOPTION && status.outcome->adoption == EAdoption::BUSY);
            assert(std::holds_alternative<CommitReceipt>(status.outcome->publication));
            assert(f.saves.acknowledge(first));
            assert(f.material_session->describe().dirty);
            return {};
        }));
        f.edit(3);
        const auto third = take(f.saves.requestSave({f.material_id}));
        f.encodeAll();
        publishWithoutAdoption(f);
        const auto latest = take(f.saves.status(third));
        const auto info = take(f.material_source->describe());
        const auto disk = std::get<CommitReceipt>(*take(f.writes.status(latest.ticket)).outcome);
        assert(
            f.material_source->accept({latest.content, info.binding, {latest.ticket.value}, *info.target, disk}) ==
            EAdoption::APPLIED
        );
        f.saves.adoptCompletions();
        assert(take(f.saves.status(second)).outcome->adoption == EAdoption::OLDER_RECEIPT);
        assert(!f.material_session->describe().dirty && f.material_session->describe().current != first_content);
        assert(take(em::MaterialCodec::decode(read(f.root / "material.luxmaterial"))).source.name == "material3");
        assert(f.saves.acknowledge(second) && f.saves.acknowledge(third));
        std::cout
            << "R05-06 withRead BUSY, early ack, later save and reversed adoption preserve latest baseline PASS\n";
    }
    void actualConflictCase(Fixture& f)
    {
        const WriteOrigin origin{f.material_id, {1}};
        int serial{};
        auto commit = [&](WriteTarget target, WriteOrigin writer, bool success) {
            f.edit(++serial);
            auto artifact = take(em::MaterialCodec::encode(take(f.material_session->capture()), identity("material")));
            const auto ticket = take(f.writes.reserve(std::move(target), writer));
            assert(f.writes.provideEncoded(ticket, std::move(artifact)));
            publishWithoutAdoption(f);
            const auto status = take(f.writes.status(ticket));
            assert(status.outcome && std::holds_alternative<CommitReceipt>(*status.outcome) == success);
            if (!success)
                assert(std::get<NotPublished>(*status.outcome).failure.code == EPersistenceError::CONFLICT);
            return ticket;
        };
        const auto first = commit(take(f.disk.resolve("material.luxmaterial")), origin, true);
        const auto observed = take(f.disk.resolve("./sub/../material.luxmaterial"));
        (void)commit(observed, origin, true);
        assert(f.writes.acknowledge(first));
        const auto controlled = read(f.root / "material.luxmaterial");
        (void)commit(observed, {f.material_id, {2}}, false);
        (void)commit(observed, {}, false);
        (void)commit(observed, {f.flow_id, {1}}, false);
        assert(read(f.root / "material.luxmaterial") == controlled);
        // A real external file replacement does not gain provenance from the coordinator.
        {
            std::ofstream external(f.root / "material.luxmaterial", std::ios::binary | std::ios::trunc);
            external << "external version";
        }
        const auto external = read(f.root / "material.luxmaterial");
        (void)commit(observed, origin, false);
        assert(read(f.root / "material.luxmaterial") == external);
        (void)commit(take(f.disk.resolve("material.luxmaterial")), origin, true);
        const auto new_chain = read(f.root / "material.luxmaterial");
        (void)commit(observed, origin, false);
        assert(read(f.root / "material.luxmaterial") == new_chain);
        (void)commit(take(f.disk.resolve("material.luxmaterial")), {f.flow_id, {1}}, true);
        const auto other = read(f.root / "material.luxmaterial");
        (void)commit(observed, origin, false);
        assert(read(f.root / "material.luxmaterial") == other);
        assert(take(em::MaterialCodec::decode(other)).source.name == "material9");
        std::cout << "R05-07 real FileArtifactStore alias/binding/anonymous/external/chain-break conflicts PASS\n";
    }
    void closeCases(Fixture& f, bool revoke = true)
    {
        f.edit(1);
        const std::array ids{f.scene_id, f.material_id, f.flow_id};
        std::vector<SaveId> requests;
        for (auto id : ids)
            requests.push_back(take(f.saves.requestSave({id})));
        if (revoke)
        {
            f.registrations.clear();
            f.scene_source.reset();
            f.material_source.reset();
            f.flow_source.reset();
        }
        for (auto id : ids)
        {
            auto permit = take(f.store.prepareClose(take(f.store.describe(id)).current));
            assert(f.store.close(permit));
        }
        std::vector<sessions::SessionId> replacements;
        std::vector<sessions::SessionInfo> before;
        for (int i{}; i < 3; ++i)
        {
            auto reservation =
                take(f.store.reserve<em::MaterialSession>({"lux.editor.material"}, lux::object::CodeLease::builtin()));
            replacements.push_back(reservation.id());
            assert(std::ranges::any_of(ids, [&](auto old) {
                return old.slot == reservation.id().slot && old.generation != reservation.id().generation;
            }));
            auto session = take(em::MaterialSession::create(reservation.id(), {}, {identity("new"), "new", {}}));
            before.push_back(session->describe());
            assert(f.store.prepare(reservation, session) && f.store.publish(reservation));
        }
        f.encodeAll(); // Snapshots remain usable after all original sessions and adapters died.
        f.publishAll();
        checkFiles(f, 1, "1");
        for (std::size_t i{}; i < requests.size(); ++i)
        {
            auto outcome = take(f.saves.status(requests[i])).outcome;
            assert(outcome && std::holds_alternative<CommitReceipt>(outcome->publication));
            assert(outcome->adoption == EAdoption::CLOSED);
            auto after = take(f.store.describe(replacements[i]));
            assert(after.current == before[i].current && after.observed == before[i].observed);
            assert(after.binding == before[i].binding && after.dirty == before[i].dirty);
            assert(f.saves.acknowledge(requests[i]));
        }
        std::cout << "X05-07/Q17 three actual sessions removed before encode; slots reused; disk retained, adoption "
                     "CLOSED PASS\n";
    }
    void capacityCases(Fixture& f)
    {
        f.edit(1);
        SaveService bounded(f.writes, {1, 1024 * 1024, 2});
        auto registration = take(bounded.registerSource(*f.material_source));
        auto first = take(bounded.requestSave({f.material_id}));
        assert(!bounded.requestSave({f.material_id}));
        auto work = take(bounded.takeEncoding());
        assert(work && !bounded.acknowledge(first));
        assert(take(bounded.requestCancel(first)) == ECancelResult::REQUESTED);
        assert(!bounded.requestSave({f.material_id}));
        assert(bounded.completeEncoding(first, work->encoding.encode({})));
        bounded.adoptCompletions();
        assert(std::holds_alternative<NotPublished>(take(bounded.status(first)).outcome->publication));
        auto second = take(bounded.requestSave({f.material_id}));
        assert(bounded.requestCancel(second));
        bounded.adoptCompletions();
        auto full = bounded.requestSave({f.material_id});
        assert(!full && full.error().code == EPersistenceError::CAPACITY);
        assert(bounded.acknowledge(first));
        auto third = take(bounded.requestSave({f.material_id}));
        assert(third != first && !bounded.status(first));
        assert(bounded.requestCancel(third));
        bounded.adoptCompletions();
        assert(bounded.acknowledge(second) && bounded.acknowledge(third));
        SaveService no_memory(f.writes, {1, 1, 2});
        auto reg = take(no_memory.registerSource(*f.material_source));
        assert(!no_memory.requestSave({f.material_id}));
        assert(f.writes.size() == 0);
        std::cout << "X05-09/Q18/Q50 actual material active/snapshot/terminal bounds, cancel+ack reuse PASS\n";
    }
    void executionCases(Fixture& f)
    {
        f.edit(1);
        auto runtime = take(process::ExecutionRuntime::create(
            {.cpu_concurrency = 2,
             .cpu_queue_capacity = 16,
             .timer = {16},
             .blocking = process::BlockingSchedulerConfig{2, 16}}
        ));
        std::vector<SaveId> ids;
        for (auto id : {f.scene_id, f.material_id, f.flow_id})
            ids.push_back(take(f.saves.requestSave({id})));
        persistence::SaveExecution execution(runtime, f.saves, f.writes, f.disk);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::ranges::any_of(ids, [&](auto id) { return take(f.saves.status(id)).stage != ESaveStage::TERMINAL; })
        )
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(runtime.collectCompletions());
            assert(execution.submitReady());
            f.saves.adoptCompletions();
            std::this_thread::yield();
        }
        for (auto id : ids)
            assert(take(f.saves.status(id)).outcome->adoption == EAdoption::APPLIED);
        checkFiles(f, 1, "1");
        std::cout << "Actual ExecutionRuntime CPU sender -> Blocking sender -> owner collection -> adoption, all three "
                     "PASS\n";
    }
    void receiptCases(Fixture& f)
    {
        const std::array sources{
            static_cast<ISaveSource*>(f.scene_source.get()),
            static_cast<ISaveSource*>(f.material_source.get()),
            static_cast<ISaveSource*>(f.flow_source.get())
        };
        const std::array ids{f.scene_id, f.material_id, f.flow_id};
        f.edit(1);
        std::vector<SaveId> requests;
        for (auto id : ids)
            requests.push_back(take(f.saves.requestSave({id})));
        f.edit(2);
        for (auto id : ids)
            requests.push_back(take(f.saves.requestSave({id})));
        f.encodeAll();
        while (auto next = take(f.writes.takeReady()))
            assert(f.writes.complete(next->ticket, f.disk.publish(*next)));
        for (std::size_t i{}; i < ids.size(); ++i)
        {
            auto info = take(sources[i]->describe());
            auto receipt = [&](SaveId id) {
                auto save = take(f.saves.status(id));
                auto write = take(f.writes.status(save.ticket));
                return SaveReceipt{
                    save.content,
                    info.binding,
                    {save.ticket.value},
                    *info.target,
                    std::get<CommitReceipt>(*write.outcome)
                };
            };
            assert(sources[i]->accept(receipt(requests[i + 3])) == EAdoption::APPLIED);
            assert(sources[i]->accept(receipt(requests[i])) == EAdoption::OLDER_RECEIPT);
            auto before = take(f.store.describe(ids[i]));
            auto wrong_history = receipt(requests[i + 3]);
            wrong_history.content.state.history.value += 1000000;
            wrong_history.order.value += 100;
            assert(sources[i]->accept(std::move(wrong_history)) == EAdoption::STALE_HISTORY);
            auto wrong_binding = receipt(requests[i + 3]);
            ++wrong_binding.binding.value;
            wrong_binding.order.value += 100;
            assert(sources[i]->accept(std::move(wrong_binding)) == EAdoption::STALE_BINDING);
            auto after = take(f.store.describe(ids[i]));
            assert(
                after.current == before.current && after.binding == before.binding && after.observed == before.observed
            );
            assert(!after.dirty);
        }
        f.saves.adoptCompletions();
        f.edit(3);
        auto save = take(f.saves.requestSave({f.material_id}));
        f.encodeAll();
        while (auto next = take(f.writes.takeReady()))
            assert(f.writes.complete(next->ticket, f.disk.publish(*next)));
        auto view = take(f.material_session->read());
        assert(view.withRead([&](const auto&) -> em::MaterialEditResult<void> {
            f.saves.adoptCompletions();
            assert(take(f.saves.status(save)).stage == ESaveStage::AWAITING_ADOPTION);
            assert(take(f.saves.requestCancel(save)) == ECancelResult::TOO_LATE);
            assert(f.material_session->describe().dirty);
            return {};
        }));
        f.saves.adoptCompletions();
        assert(take(f.saves.status(save)).outcome->adoption == EAdoption::APPLIED);
        assert(!f.material_session->describe().dirty);
        std::cout
            << "X05-04/Q13/Q14 three models reverse receipts, stale history/binding; reading delays adoption PASS\n";
    }
    void identityCases(Fixture& f)
    {
        auto insert = [&] {
            ef::FlowEditBatch batch{f.flow_session->describe().current, "mixed identity", {}};
            batch.edits.emplace_back(ef::FlowRename{"issued"});
            batch.edits.emplace_back(
                ef::FlowInsertNode{lux::object::CodeLease::builtin(), std::make_unique<lux::flowforge::BranchNode>()}
            );
            batch.edits.emplace_back(
                ef::FlowAddVariable{"variable", "bool", {lux::flowforge::EFlowLiteralKind::BOOLEAN, "true"}}
            );
            return take(f.flow_session->apply(std::move(batch)));
        };
        const auto first = insert();
        const auto before = take(f.flow_session->capture()).source();
        auto request = take(
            f.saves.requestSave({f.flow_id, ESaveMode::SAVE_AS, take(f.disk.resolve("ids-copy")), identity("ids-copy")})
        );
        f.encodeAll();
        f.publishAll();
        assert(take(f.saves.status(request)).outcome->adoption == EAdoption::APPLIED);
        auto after = take(f.flow_session->capture()).source();
        assert(after.id == identity("ids-copy") && after.nodes == before.nodes && after.variables == before.variables);
        assert(f.flow_session->describe().current == first.content);
        assert(f.flow_session->undo() && f.flow_session->redo());
        assert(take(f.flow_session->capture()).source() == after);
        assert(f.flow_session->undo());
        const auto fresh = insert();
        assert(fresh.inserted.nodes.front().value > first.inserted.nodes.front().value);
        assert(fresh.inserted.variables.front() > first.inserted.variables.front());
        auto current = take(f.flow_session->capture()).source();
        auto old_node =
            std::ranges::find(before.nodes, first.inserted.nodes.front(), &lux::flowforge::FlowSourceNode::id);
        auto new_node =
            std::ranges::find(current.nodes, fresh.inserted.nodes.front(), &lux::flowforge::FlowSourceNode::id);
        assert(old_node != before.nodes.end() && new_node != current.nodes.end());
        std::uint64_t highest{};
        for (const auto* pins : {&old_node->inputs, &old_node->outputs})
            for (const auto& pin : *pins)
                highest = (std::max)(highest, pin.id.value);
        for (const auto* pins : {&new_node->inputs, &new_node->outputs})
            for (const auto& pin : *pins)
                assert(pin.id.value > highest);
        std::cout << "X05-05 Flow Save As retains nodes/all pins/variables and History; Undo/Redo then fresh mixed "
                     "insert keeps high water PASS\n";
    }
    void conflictCases(Fixture& f)
    {
        auto reservation =
            take(f.store.reserve<em::MaterialSession>({"lux.editor.material"}, lux::object::CodeLease::builtin()));
        const auto id = reservation.id();
        auto session = take(em::MaterialSession::create(
            id,
            sessions::BoundSource{identity("material"), "material.luxmaterial"},
            {identity("material"), "other-copy", {}}
        ));
        auto* other = session.get();
        assert(f.store.prepare(reservation, session) && f.store.publish(reservation));
        em::MaterialSaveSource source(
            f.store.access<em::MaterialSession>(),
            take(f.store.key<em::MaterialSession>(id)),
            take(f.disk.resolve("./sub/../material.luxmaterial")),
            {1}
        );
        auto registration = take(f.saves.registerSource(source));
        f.edit(1);
        auto first = take(f.saves.requestSave({f.material_id}));
        auto second = take(f.saves.requestSave({id}));
        const auto before = other->describe();
        f.encodeAll();
        f.publishAll();
        assert(take(f.saves.status(first)).outcome->adoption == EAdoption::APPLIED);
        auto failed = take(f.saves.status(second)).outcome;
        assert(std::get<NotPublished>(failed->publication).failure.code == EPersistenceError::CONFLICT);
        assert(failed->adoption == EAdoption::NONE);
        assert(other->describe().current == before.current && other->describe().dirty == before.dirty);
        assert(take(em::MaterialCodec::decode(read(f.root / "material.luxmaterial"))).source.name == "material1");
        std::cout << "X05-06 two actual working copies, canonical aliases, real file conflict without adopting foreign "
                     "version PASS\n";
    }
    void measurementCases(Fixture& f)
    {
        const auto started = std::chrono::steady_clock::now();
        const auto allocations_before = allocation_calls.load();
        std::size_t encoded_bytes{}, captures{}, terminal_peak{};
        for (int iteration{1}; iteration <= 24; ++iteration)
        {
            f.edit(iteration);
            std::array<SaveId, 3> requests;
            const std::array ids{f.scene_id, f.material_id, f.flow_id};
            for (std::size_t i{}; i < ids.size(); ++i)
                requests[i] = take(f.saves.requestSave({ids[i]}));
            while (auto work = take(f.saves.takeEncoding()))
            {
                auto bytes = take(work->encoding.encode({}));
                encoded_bytes += bytes.bytes.size();
                ++captures;
                assert(f.saves.completeEncoding(work->id, std::move(bytes)));
            }
            f.publishAll();
            terminal_peak = (std::max)(terminal_peak, f.writes.size());
            for (auto id : requests)
                assert(f.saves.acknowledge(id));
            assert(f.writes.size() == 0);
        }
        auto micros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
        std::size_t peak_working_set{};
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS memory{sizeof(memory)};
        assert(GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)));
        peak_working_set = memory.PeakWorkingSetSize;
#endif
        std::cout << "Q50 persistence measurement: saves=" << captures << " us=" << micros
                  << " executable_plain_allocations=" << allocation_calls.load() - allocations_before
                  << " encoded_owned_bytes=" << encoded_bytes << " peak_process_working_set=" << peak_working_set
                  << " peak_retained_tickets=" << terminal_peak << " tickets_after_ack=" << f.writes.size() << '\n';
        assert(captures == 72 && terminal_peak == 3);
        std::cout << "Capture performed once per save; worker owns same snapshot, no live-session clone; no speedup "
                     "claim PASS\n";
    }
    void decodedCases(Fixture& f)
    {
        f.edit(1);
        for (auto id : {f.scene_id, f.material_id, f.flow_id})
            (void)take(f.saves.requestSave({id}));
        f.encodeAll();
        f.publishAll();
        auto s = take(es::SceneCodec::decode(read(f.root / "scene.pak")));
        auto m = take(em::MaterialCodec::decode(read(f.root / "material.luxmaterial")));
        auto g = take(ef::FlowCodec::decode(read(f.root / "flow.luxflow")));
        auto sr = take(f.store.reserve<es::SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin()));
        auto mr = take(f.store.reserve<em::MaterialSession>({"lux.editor.material"}, lux::object::CodeLease::builtin()));
        auto gr = take(f.store.reserve<ef::FlowSession>({"lux.editor.flowforge"}, lux::object::CodeLease::builtin()));
        auto scene =
            take(std::move(s).createSession(sr.id(), {}, take(simulation::ecs::ComponentSchemaSet::build({}))));
        auto material = take(std::move(m).createSession(mr.id(), {}));
        auto flow = take(std::move(g).createSession(gr.id(), {}));
        assert(take(scene->capture()).objects().size() == 1);
        assert(take(material->capture()).source().name == "material1");
        assert(take(flow->capture()).source().name == "flow1");
        assert(f.store.prepare(sr, scene) && f.store.publish(sr));
        assert(f.store.prepare(mr, material) && f.store.publish(mr));
        assert(f.store.prepare(gr, flow) && f.store.publish(gr));
        assert(f.store.size() == 6);
        std::cout << "Three concrete decode -> owning PreparedData -> owner SessionStore construction PASS\n";
    }
    void saveAsCases(Fixture& f)
    {
        f.edit(1);
        const std::array ids{f.scene_id, f.material_id, f.flow_id};
        const std::array names{"scene-copy.pak", "material-copy.luxmaterial", "flow-copy.luxflow"};
        std::array<sessions::SessionInfo, 3> before;
        std::array<SaveId, 3> requests;
        for (std::size_t i{}; i < ids.size(); ++i)
        {
            before[i] = take(f.store.describe(ids[i]));
            auto target = take(f.disk.resolve(std::string("blocked") + std::to_string(i) + "/copy"));
            requests[i] = take(f.saves.requestSave({ids[i], ESaveMode::SAVE_AS, target, identity(names[i])}));
            assert(take(f.store.describe(ids[i])).admission == sessions::EEditAdmission::REBINDING);
            assert(!f.store.prepareClose(before[i].current));
            assert(!f.saves.requestSave({ids[i]}));
            std::ofstream(f.root / ("blocked" + std::to_string(i))) << "real parent path failure";
        }
        em::MaterialEditBatch blocked{before[1].current, "blocked", {}};
        blocked.edits.emplace_back(em::MaterialRename{"must not apply"});
        assert(!f.material_session->apply(std::move(blocked)));
        f.encodeAll();
        f.publishAll();
        for (std::size_t i{}; i < ids.size(); ++i)
        {
            const auto after = take(f.store.describe(ids[i]));
            const auto status = take(f.saves.status(requests[i]));
            assert(status.outcome && std::holds_alternative<NotPublished>(status.outcome->publication));
            assert(
                after.binding == before[i].binding && after.current == before[i].current &&
                after.dirty == before[i].dirty
            );
            assert(after.observed == before[i].observed && after.admission == sessions::EEditAdmission::AVAILABLE);
            assert(f.saves.acknowledge(requests[i]));
            requests[i] = take(
                f.saves.requestSave({ids[i], ESaveMode::SAVE_AS, take(f.disk.resolve(names[i])), identity(names[i])})
            );
        }
        f.encodeAll();
        f.publishAll();
        for (std::size_t i{}; i < ids.size(); ++i)
        {
            const auto after = take(f.store.describe(ids[i]));
            assert(after.current == before[i].current && !after.dirty);
            assert(after.binding->asset == identity(names[i]));
            assert(take(f.saves.status(requests[i])).outcome->adoption == EAdoption::APPLIED);
            assert(f.saves.acknowledge(requests[i]));
        }
        auto scene = take(es::SceneCodec::decode(read(f.root / names[0])));
        assert(scene.source.scene->id() == identity(names[0]));
        assert(scene.source.partitions[0]->objectAt(0).id().value == identity("1").uuid());
        assert(take(em::MaterialCodec::decode(read(f.root / names[1]))).source.id == identity(names[1]));
        assert(take(ef::FlowCodec::decode(read(f.root / names[2]))).source.id == identity(names[2]));
        assert(f.scene_session->undo() && f.material_session->undo() && f.flow_session->undo());
        assert(
            f.scene_session->describe().dirty && f.material_session->describe().dirty &&
            f.flow_session->describe().dirty
        );
        assert(f.scene_session->redo() && f.material_session->redo() && f.flow_session->redo());
        assert(
            !f.scene_session->describe().dirty && !f.material_session->describe().dirty &&
            !f.flow_session->describe().dirty
        );
        // New ordinary saves use the adopted target; author history was not replaced to implement Save As.
        f.edit(2);
        for (std::size_t i{}; i < ids.size(); ++i)
            requests[i] = take(f.saves.requestSave({ids[i]}));
        f.encodeAll();
        f.publishAll();
        for (auto id : requests)
            assert(take(f.saves.status(id)).outcome->adoption == EAdoption::APPLIED);
        assert(take(es::SceneCodec::decode(read(f.root / names[0]))).source.partitions[0]->objectCount() == 2);
        std::cout << "X05-05/Q15/Q16 three Save As: real failure retains full state; success keeps history/author IDs; "
                     "subsequent save PASS\n";
        f.edit(3);
        for (std::size_t i{}; i < ids.size(); ++i)
        {
            before[i] = take(f.store.describe(ids[i]));
            requests[i] = take(f.saves.requestSave(
                {ids[i],
                 ESaveMode::EXPORT_COPY,
                 take(f.disk.resolve(std::string("export-") + names[i])),
                 identity(std::string("export-") + names[i])}
            ));
        }
        f.encodeAll();
        f.publishAll();
        for (std::size_t i{}; i < ids.size(); ++i)
        {
            const auto after = take(f.store.describe(ids[i]));
            assert(after.current == before[i].current && after.binding == before[i].binding);
            assert(after.dirty == before[i].dirty && after.observed == before[i].observed);
            assert(take(f.saves.status(requests[i])).outcome->adoption == EAdoption::NONE);
        }
        std::cout << "X05-05 three Export Copy: binding, current, observed, dirty unchanged PASS\n";
    }
    struct LateStore final : IArtifactStore
    {
        storage::FileArtifactStore& disk;
        bool retired{};
        explicit LateStore(storage::FileArtifactStore& value) : disk(value) {}
        PersistenceResult<WriteTarget> resolve(std::string_view path) override
        {
            return disk.resolve(path);
        }
        VPublicationOutcome publish(const PublicationQuery&, std::stop_token) override
        {
            return PublicationUnknown{{EPersistenceError::IO, "writer has not retired"}, "controlled-late-writer"};
        }
        Reconciliation reconcile(const PublicationQuery& work) override
        {
            if (!retired)
                return {false, publish(work, {})};
            return {true, disk.publish(work)};
        }
    };
    void unknownCases(Fixture& f)
    {
        f.edit(1);
        LateStore late{f.disk};
        const std::array ids{f.scene_id, f.material_id, f.flow_id};
        const std::array names{"unknown-scene.pak", "unknown-material", "unknown-flow"};
        std::array<sessions::SessionInfo, 3> before;
        std::vector<SaveId> saves;
        for (std::size_t i{}; i < ids.size(); ++i)
        {
            before[i] = take(f.store.describe(ids[i]));
            saves.push_back(take(
                f.saves.requestSave({ids[i], ESaveMode::SAVE_AS, take(f.disk.resolve(names[i])), identity(names[i])})
            ));
        }
        f.encodeAll();
        while (auto work = take(f.writes.takeReady()))
            assert(f.writes.complete(work->ticket, late.publish(*work, {})));
        f.saves.adoptCompletions();
        for (std::size_t i{}; i < ids.size(); ++i)
        {
            auto info = take(f.store.describe(ids[i]));
            assert(info.current == before[i].current && info.binding == before[i].binding);
            assert(info.observed == before[i].observed && info.dirty == before[i].dirty);
            assert(info.admission == sessions::EEditAdmission::AVAILABLE);
            assert(!f.saves.acknowledge(saves[i]));
            assert(!f.writes.reconcile(take(f.saves.status(saves[i])).ticket, late));
        }
        f.edit(2);
        late.retired = true;
        for (auto save : saves)
            assert(f.writes.reconcile(take(f.saves.status(save)).ticket, late));
        f.saves.adoptCompletions();
        for (std::size_t i{}; i < ids.size(); ++i)
        {
            auto status = take(f.saves.status(saves[i]));
            assert(status.outcome && std::holds_alternative<CommitReceipt>(status.outcome->publication));
            assert(status.outcome->adoption == EAdoption::NONE);
            auto info = take(f.store.describe(ids[i]));
            assert(info.binding == before[i].binding && info.current != before[i].current && info.dirty);
            assert(f.saves.acknowledge(saves[i]));
        }
        assert(take(es::SceneCodec::decode(read(f.root / names[0]))).source.partitions[0]->objectCount() == 1);
        assert(take(em::MaterialCodec::decode(read(f.root / names[1]))).source.name == "material1");
        assert(take(ef::FlowCodec::decode(read(f.root / names[2]))).source.name == "flow1");
        std::cout << "X05-03/X05-05 three unknown Save As: real late files, permit release, no stale rebind PASS\n";
    }
}
int main(int argc, char** argv)
{
    assert(argc == 2 || argc == 3);
    const auto root = std::filesystem::absolute(argv[1]) /
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    Fixture f(root);
    const std::string_view scenario = argc == 3 ? argv[2] : "models";
    if (scenario.starts_with("r11-input-"))
    {
        r11InputCleanup(f, scenario.substr(10));
        return 0;
    }
    if (scenario.starts_with("r2-accept-"))
    {
        r2AcceptCase(f, scenario.substr(10));
        return 0;
    }
    if (scenario == "r2-admission")
    {
        for (const bool capture : {false, true})
            for (const std::string_view action : {"success", "throw", "revoke"})
            {
                Fixture admission(root / (std::string(capture ? "capture-" : "describe-") + std::string(action)));
                r2AdmissionCase(admission, capture, action);
            }
        r2ExecutionFailure(f);
        return 0;
    }
    if (scenario == "r1-revoke")
    {
        revokeCase(f);
        return 0;
    }
    if (scenario == "r1-callbacks")
    {
        callbackCases(f);
        return 0;
    }
    if (scenario == "r1-reading")
    {
        delayedReadCase(f);
        return 0;
    }
    if (scenario == "r1-conflicts")
    {
        actualConflictCase(f);
        return 0;
    }
    if (scenario == "r1-recursive")
    {
        recursiveCase(f);
        return 0;
    }
    if (scenario == "r1-chain" || scenario == "r1-chain-control")
    {
        acknowledgedChainCase(f, scenario == "r1-chain");
        return 0;
    }
    if (scenario == "save-as")
    {
        saveAsCases(f);
        return 0;
    }
    if (scenario == "unknown")
    {
        unknownCases(f);
        return 0;
    }
    if (scenario == "order")
    {
        orderCases(f);
        return 0;
    }
    if (scenario == "close")
    {
        closeCases(f);
        return 0;
    }
    if (scenario == "close-key")
    {
        closeCases(f, false);
        return 0;
    }
    if (scenario == "capacity")
    {
        capacityCases(f);
        return 0;
    }
    if (scenario == "execution")
    {
        executionCases(f);
        return 0;
    }
    if (scenario == "decoded")
    {
        decodedCases(f);
        return 0;
    }
    if (scenario == "receipts")
    {
        receiptCases(f);
        return 0;
    }
    if (scenario == "identities")
    {
        identityCases(f);
        return 0;
    }
    if (scenario == "conflict")
    {
        conflictCases(f);
        return 0;
    }
    if (scenario == "measure")
    {
        measurementCases(f);
        return 0;
    }
    f.edit(1);
    const auto scene_stamp = f.scene_session->describe().current;
    const auto material_stamp = f.material_session->describe().current;
    const auto flow_stamp = f.flow_session->describe().current;
    std::vector<SaveId> requests;
    for (auto id : {f.scene_id, f.material_id, f.flow_id})
        requests.push_back(take(f.saves.requestSave({id})));
    f.edit(2); // Frozen jobs must remain usable while all live models continue editing.
    f.encodeAll();
    f.publishAll();
    for (auto id : requests)
    {
        auto status = take(f.saves.status(id));
        assert(status.outcome && status.outcome->adoption == EAdoption::APPLIED);
        assert(std::holds_alternative<CommitReceipt>(status.outcome->publication));
    }
    assert(
        f.scene_session->describe().dirty && f.material_session->describe().dirty && f.flow_session->describe().dirty
    );
    auto scene_data = take(es::SceneCodec::decode(read(root / "scene.pak")));
    assert(scene_data.source.partitions.size() == 1 && scene_data.source.partitions[0]->objectCount() == 1);
    assert(scene_data.source.partitions[0]->objectAt(0).payloadAt(0)[0] == std::byte{1});
    auto mat_data = take(em::MaterialCodec::decode(read(root / "material.luxmaterial")));
    auto flow_data = take(ef::FlowCodec::decode(read(root / "flow.luxflow")));
    assert(mat_data.source.name == "material1" && flow_data.source.name == "flow1");
    assert(f.scene_session->undo() && f.material_session->undo() && f.flow_session->undo());
    assert(f.scene_session->describe().current == scene_stamp && !f.scene_session->describe().dirty);
    assert(f.material_session->describe().current == material_stamp && !f.material_session->describe().dirty);
    assert(f.flow_session->describe().current == flow_stamp && !f.flow_session->describe().dirty);
    std::cout << "X05-04 actual three sessions: captured baseline, continued edits, exact Undo clean; real codec bytes "
                 "PASS\n";
    for (auto id : requests)
        assert(f.saves.acknowledge(id));
    std::cout << "three author sessions share one Store, SaveService and WriteCoordinator: PASS\n";
}
