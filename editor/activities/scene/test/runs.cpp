#include <lux/engine/editor/scene/RunController.hpp>
#include <lux/engine/editor/scene/ModelCreationOperation.hpp>
#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/editor/editing/scene/FieldEdit.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <cassert>
#include <cstdio>
#include <thread>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::scene;
    using namespace std::chrono_literals;
    namespace ecs = simulation::ecs;
    template <class T> auto take(T value, std::source_location where = std::source_location::current())
    {
        if (!value)
            std::fprintf(stderr, "Unexpected failure at %s:%u\n", where.file_name(), where.line());
        assert(value);
        return std::move(*value);
    }
    uuids::uuid uuid(std::string_view name)
    {
        return uuids::uuid_name_generator(*uuids::uuid::from_string("01234567-89ab-cdef-0123-456789abcdef"))(name);
    }
    struct Probe final
    {
        inline static constexpr std::string_view supported[]{"*"};
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "test.run.probe",
            .version = 1,
            .supported_world_types = supported
        };
        struct Facts final
        {
            std::uint64_t maintenance{}, publication{};
            bool fail{}, pending{};
        };
        inline static std::function<void()> installing;
        inline static bool reject_install{};
        inline static bool custom_failure{};
        inline static std::function<void()> copying_failure;
        struct Failure final
        {
            int reason{731};
            Failure() = default;
            Failure(const Failure& other) : reason(other.reason)
            {
                if (copying_failure)
                    copying_failure();
            }
        };
        inline static std::function<void()> maintaining;
        inline static bool* in_flight{};
        inline static unsigned destroyed{};
        Facts& facts;
        explicit Probe(ecs::Registry& registry) : facts(registry.ctx().emplace<Facts>()) {}
        ~Probe()
        {
            ++destroyed;
        }
    };
    lux::scene::SceneSystemRegistration probeRegistration()
    {
        return {
            .type = system::systemTypeId(Probe::Description.canonical_name),
            .cpp_type = cxx::typeToken<Probe>(),
            .description = &Probe::Description,
            .install = +[](lux::scene::SceneSystemInstaller& installer, lux::scene::SceneSystemDescription description
                        ) noexcept -> cxx::expected<void, lux::scene::SceneSystemBuildFailure> {
                auto system = installer.emplaceSystem<Probe>(description.instanceId(), installer.registry());
                if (!system)
                    return cxx::unexpected(system.error());
                if (Probe::installing)
                    Probe::installing();
                if (Probe::reject_install)
                    return cxx::unexpected(lux::scene::SceneSystemBuildFailure{
                        .code = lux::scene::ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE,
                        .system = description.instanceId(),
                        .cause = 419
                    });
                auto maintenance = installer.addMaintenanceTask<Probe>(
                    description.instanceId(),
                    [](Probe& probe) noexcept -> lux::scene::SceneStageResult {
                        ++probe.facts.maintenance;
                        if (Probe::maintaining)
                            Probe::maintaining();
                        if (Probe::in_flight && *Probe::in_flight)
                            return lux::scene::ESceneProgress::PENDING;
                        return probe.facts.pending ? lux::scene::ESceneProgress::PENDING
                                                   : lux::scene::ESceneProgress::COMPLETE;
                    }
                );
                if (!maintenance)
                    return maintenance;
                return installer.addPublicationTask<Probe>(
                    description.instanceId(),
                    [](Probe& probe) noexcept -> lux::scene::SceneStageResult {
                        ++probe.facts.publication;
                        if (probe.facts.fail && Probe::custom_failure)
                            return cxx::unexpected(lux::scene::SceneExecutionFailure{
                                lux::scene::ESceneExecutionError::SYSTEM_FAILURE,
                                {},
                                Probe::Failure{}
                            });
                        if (probe.facts.fail)
                            return cxx::unexpected(lux::scene::SceneExecutionFailure{
                                lux::scene::ESceneExecutionError::SYSTEM_FAILURE,
                                {},
                                731
                            });
                        return lux::scene::ESceneProgress::COMPLETE;
                    }
                );
            }
        };
    }
    struct Fixture final
    {
        // Disable diagnostic history so taskInfos() below measures live task records.
        process::ExecutionRuntime execution{take(process::ExecutionRuntime::create({1, 64, 64, {32}, {}, 0}))};
        std::unique_ptr<lux::scene::SceneRuntime> runtime{take(lux::scene::SceneRuntime::create(execution, {0, 1024}))};
        RunStore runs{*runtime, execution, 4};
        RunController controller{runs};
        sessions::SessionStore authors{4};
        ecs::ComponentSchemaSet schemas;
        std::shared_ptr<const simulation::SimulationSystemRegistry> systems{
            std::make_shared<simulation::SimulationSystemRegistry>()
        };
        std::vector<lux::scene::SceneSystemRegistration> registrations{
            lux::scene::worldLoadingSystemRegistration(),
            lux::scene::transformSystemRegistration(),
            probeRegistration()
        };
        SceneSession* author{};
        sessions::SessionId author_id;
        world::WorldObjectId object{uuid("object")};

        explicit Fixture(std::size_t capacity = 4, SceneSessionLimits limits = {}) : runs(*runtime, execution, capacity)
        {
            std::vector<ecs::ComponentSchema> types;
            for (auto group :
                 {ecs::transformComponentSchemas(), ecs::hierarchyComponentSchemas(), ecs::visualComponentSchemas()})
                for (auto schema : group)
                    if (schema.snapshot == ecs::EComponentSnapshotPolicy::COPY)
                        types.push_back(std::move(schema));
            schemas = take(ecs::ComponentSchemaSet::build(std::move(types)));
            std::vector<world::WorldDataSchemaId> ids;
            for (const auto& schema : schemas.all())
                ids.push_back(world::worldDataSchemaId(schema.id.name));
            lux::scene::SceneDescriptionBuilder builder;
            lux::scene::WorldLoadingConfiguration loading{{{0}}};
            std::vector<std::byte> payload;
            assert(registrations[0].configuration.encode(&loading, payload));
            assert(builder.addSystem(
                {1},
                "loading",
                registrations[0].type,
                1,
                registrations[0].description->configuration_schema_name,
                1,
                payload
            ));
            auto transform = take(lux::scene::makeTransformSystemConfiguration(64, {512, 65536}));
            assert(builder.addSystem(
                {2},
                "transform",
                registrations[1].type,
                1,
                registrations[1].description->configuration_schema_name,
                1,
                transform
            ));
            assert(builder.addSystem({3}, "probe", registrations[2].type, 1, {}, 0));
            auto description = take(std::move(builder).buildResolved());
            auto simulation = take(std::move(simulation::SimulationDescriptionBuilder{}).build());
            auto package = take(lux::scene::createScenePackage(
                asset::AssetId{uuid("run-scene")},
                "run",
                ids,
                std::make_shared<const simulation::SimulationDescription>(std::move(simulation)),
                description
            ));
            auto reservation =
                take(authors.reserve<SceneSession>({"lux.editor.scene"}, contracts::CodeLease::builtin()));
            author_id = reservation.id();
            auto source = take(SceneSource::create(package, schemas));
            auto session = take(SceneSession::create(
                author_id,
                sessions::BoundSource{package.scene->id(), "scene.lux"},
                std::move(source),
                limits
            ));
            author = session.get();
            assert(authors.prepare(reservation, session) && authors.publish(reservation));
            ecs::WorldEntityMap identities;
            ecs::Transform3D pose;
            pose.translation.x() = 10;
            SceneEditBatch batch{author->describe().current, "S10", {}};
            batch.edits.push_back(
                SceneCreateObject{{object, {0}, {take(encodeSceneValue(pose, schemas, identities, 4096))}}}
            );
            assert(author->apply(std::move(batch)));
        }
        RunEnvironment environment() const
        {
            return {schemas, systems, registrations};
        }
        void frame()
        {
            assert(execution.collectCompletions());
            assert(execution.dispatchTaskEvents());
            assert(runtime->driveFrame());
            assert(runs.update());
        }
        template <class Predicate>
        void until(Predicate ready, std::source_location where = std::source_location::current())
        {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!ready())
            {
                frame();
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    std::fprintf(stderr, "Timed out at %s:%u\n", where.file_name(), where.line());
                    std::abort();
                }
                std::this_thread::yield();
            }
        }
        RunId start()
        {
            auto prepared = take(controller.prepare(*author, environment()));
            until([&] { return prepared->ready(); });
            const auto id = take(controller.adopt(*prepared));
            until([&] { return entity(id) != ecs::NullEntity; });
            return id;
        }
        ecs::Entity entity(RunId id)
        {
            auto registry = runs.inspect().borrow(id);
            if (!registry)
                return ecs::NullEntity;
            return registry->get().ctx().get<lux::scene::WorldResidency>().identities().entity(object);
        }
        const Probe::Facts& facts(RunId id)
        {
            return take(runs.inspect().borrow(id)).get().ctx().get<Probe::Facts>();
        }
        void setAuthor(double x)
        {
            const auto before = author->describe();
            SceneEditBatch edit{before.current, "S12", {}};
            edit.edits.push_back(SceneSetField::make<ecs::Transform3D>(
                {{author_id, before.current.state.history, object},
                 ecs::componentSchemaId("lux.ecs.Transform3D"),
                 "translation"},
                Eigen::Vector3d{x, 0, 0}
            ));
            assert(author->apply(std::move(edit)));
        }
        void stop(RunId id)
        {
            auto ticket = take(runs.stop(id));
            assert(!ticket.complete());
            until([&] { return ticket.complete(); });
            assert(runs.update());
            assert(take(runs.info(id)).state == ERunState::STOPPED || take(runs.info(id)).state == ERunState::FAILED);
            assert(runs.acknowledgeStop(id));
        }
    };
    void isolation()
    {
        Fixture f;
        const auto captured = f.author->describe();
        auto prepared = take(f.controller.prepare(*f.author, f.environment()));
        f.setAuthor(12);
        const auto modified = f.author->describe();
        f.until([&] { return prepared->ready(); });
        auto id = take(f.controller.adopt(*prepared));
        assert(!f.controller.adopt(*prepared));
        assert(take(f.runs.info(id)).provenance.content == captured.current);
        f.until([&] { return f.entity(id) != ecs::NullEntity; });
        assert(f.runs.pause(id));
        f.until([&] { return take(f.runs.info(id)).state == ERunState::PAUSED; });
        const auto ref = take(f.runs.inspect().reference(id, f.entity(id)));
        const auto& first = take(f.runs.inspect().borrow(id)).get();
        assert(first.get<ecs::Transform3D>(ref.entity).translation.x() == 10);
        auto& editing = take(f.runs.debugEditing(id)).get();
        auto target = take(editing.writeTarget(ref.entity));
        assert(editing.setField<ecs::Transform3D>(
            target,
            "translation",
            "Run edit",
            [](auto& value) { return &value.translation; },
            Eigen::Vector3d{20, 0, 0}
        ));
        assert(take(f.runs.debugHistory(id)).get().undo());
        assert(first.get<ecs::Transform3D>(ref.entity).translation.x() == 10);
        assert(take(f.runs.debugHistory(id)).get().redo());
        f.frame();
        assert(
            take(f.runs.inspect().borrow(id)).get().get<ecs::WorldTransform3D>(ref.entity).value.translation().x() == 20
        );
        const auto author_snapshot = take(f.author->capture());
        f.stop(id);
        assert(!f.runs.inspect().contains(ref));
        const auto after = f.author->describe();
        assert(
            after.current == modified.current && after.observed == modified.observed && after.dirty == modified.dirty
        );
        assert(after.binding == modified.binding);
        auto next = take(f.author->capture());
        assert(next.objects().size() == author_snapshot.objects().size());
        assert(next.objects()[0].components[0].bytes == author_snapshot.objects()[0].components[0].bytes);
        std::puts(
            "PASS X06-05 frozen provenance, author S12 and run S20 isolated, real pause Undo/Redo, stop no writeback"
        );
    }
    void controls()
    {
        Fixture f;
        const auto first = f.start(), second = f.start();
        assert(first != second);
        assert(f.runs.pause(first));
        f.until([&] { return take(f.runs.info(first)).state == ERunState::PAUSED; });
        const auto paused = take(f.runs.info(first)).progress.time;
        const auto before = f.facts(first).maintenance;
        const auto view1 = f.runs.inspect(), view2 = f.runs.inspect();
        assert(view1.borrow(first) && view2.borrow(first));
        f.frame();
        assert(f.facts(first).maintenance == before + 1);
        assert(take(f.runs.info(first)).progress.time.step_index == paused.step_index);
        std::vector<StepTicket> tickets;
        for (unsigned i{}; i < 32; ++i)
            tickets.push_back(take(f.runs.step(first)));
        const auto full = f.runs.step(first);
        assert(
            !full &&
            std::get<lux::scene::ESceneRuntimeError>(std::get<lux::scene::SceneRuntimeFailure>(full.error().cause).cause
            ) == lux::scene::ESceneRuntimeError::CAPACITY
        );
        assert(!f.runs.resume(first));
        for (unsigned i{}; i < tickets.size(); ++i)
        {
            assert(take(f.runs.stepStatus(tickets[i])).state == lux::scene::ESceneStepState::QUEUED);
            f.frame();
            assert(take(f.runs.stepStatus(tickets[i])).state == lux::scene::ESceneStepState::COMPLETED);
            assert(take(f.runs.info(first)).progress.time.step_index == paused.step_index + i + 1);
            assert(tickets[i].step.simulation_completed == paused.step_index + i + 1);
        }
        for (auto ticket : tickets)
            assert(f.runs.acknowledgeStep(ticket));
        assert(!f.runs.stepStatus(tickets.front()));
        std::this_thread::sleep_for(50ms);
        const auto before_resume = take(f.runs.info(first)).progress.time;
        assert(f.runs.resume(first));
        f.frame();
        const auto resumed = take(f.runs.info(first)).progress.time;
        assert(resumed.step_index == before_resume.step_index + 1 && resumed.delta == 16ms);
        f.stop(first);
        assert(!f.runs.step(first));
        const auto replacement = f.start();
        assert(replacement.slot == first.slot && replacement.generation != first.generation);
        assert(!f.runs.step(first) && !f.runs.stop(first));
        f.stop(replacement);
        f.stop(second);
        std::puts("PASS X06-01/02/03 one real frame owner, two consumers, pause maintenance, bounded FIFO, resume dt, "
                  "stale RunId");
    }
    void failure()
    {
        Fixture f;
        const auto id = f.start();
        assert(f.runs.pause(id));
        f.until([&] { return take(f.runs.info(id)).state == ERunState::PAUSED; });
        const auto instance = take(f.runs.info(id)).instance;
        auto& facts = take(f.runtime->borrowInstance(instance)).get().ctx().get<Probe::Facts>();
        facts.fail = true;
        auto ticket = take(f.runs.step(id));
        assert(f.runtime->driveFrame());
        const auto status = take(f.runs.stepStatus(ticket));
        assert(status.state == lux::scene::ESceneStepState::FAILED && !status.result);
        assert(f.runs.update());
        f.until([&] { return take(f.runs.info(id)).state == ERunState::FAILED; });
        assert(!take(f.runs.info(id)).result);
        assert(f.runs.acknowledgeStop(id));
        auto cancelled = take(f.controller.prepare(*f.author, f.environment()));
        cancelled->cancel();
        f.until([&] { return cancelled->ready(); });
        const auto rejected = f.controller.adopt(*cancelled);
        assert(!rejected && std::get<ERunError>(rejected.error().cause) == ERunError::CANCELLED);
        auto rejected_build = take(f.controller.prepare(*f.author, f.environment()));
        f.until([&] { return rejected_build->ready(); });
        Probe::reject_install = true;
        const auto failed = f.controller.adopt(*rejected_build);
        Probe::reject_install = false;
        assert(!failed);
        const auto& runtime_error = std::get<lux::scene::SceneRuntimeFailure>(failed.error().cause);
        assert(
            std::any_cast<int>(std::get<lux::scene::SceneBuildFailure>(runtime_error.cause).scene_system.cause) == 419
        );
        assert(!f.controller.adopt(*rejected_build));
        const auto replacement = f.start();
        f.stop(replacement);
        std::puts("PASS X06-03 failed actual step never succeeds; cancelled preparation never publishes RunId");
    }
    bool lateResults(std::string_view mode)
    {
        Fixture f;
        const auto id = f.start();
        assert(f.runs.pause(id));
        f.until([&] { return take(f.runs.info(id)).state == ERunState::PAUSED; });
        const auto before = f.author->describe();
        const auto source = take(f.author->capture());
        const auto initial = take(f.runs.info(id));
        std::vector<StepTicket> tickets;
        if (mode == "r1-mixed")
        {
            tickets.push_back(take(f.runs.step(id)));
            // Observe frame progress, never the ticket, before retirement.
            f.until([&] {
                return take(f.runs.info(id)).progress.publication_completed >=
                       tickets.front().step.simulation_completed;
            });
        }
        if (mode == "r1-failed")
        {
            take(f.runtime->borrowInstance(initial.instance)).get().ctx().get<Probe::Facts>().fail = true;
            tickets.push_back(take(f.runs.step(id)));
            f.until([&] { return take(f.runs.info(id)).state == ERunState::FAILED; });
        }
        else
        {
            tickets.push_back(take(f.runs.step(id)));
            tickets.push_back(take(f.runs.step(id)));
        }
        auto stop = take(f.runs.stop(id));
        f.until([&] { return stop.complete(); });
        assert(f.runs.update());
        const auto info = take(f.runs.info(id));
        if (mode == "r1-queued")
            assert(info.progress.time.step_index == initial.progress.time.step_index);
        if (mode == "r1-mixed")
            assert(info.progress.time.step_index == initial.progress.time.step_index + 1);
        assert(info.state == (mode == "r1-failed" ? ERunState::FAILED : ERunState::STOPPED));
        assert(!f.runtime->borrowClock(initial.instance));
        const auto after = f.author->describe();
        assert(after.current == before.current && after.observed == before.observed && after.dirty == before.dirty);
        assert(after.binding == before.binding);
        const auto next = take(f.author->capture());
        assert(next.objects().size() == source.objects().size());
        assert(next.objects()[0].components[0].bytes == source.objects()[0].components[0].bytes);
        bool passed = true;
        for (std::size_t i{}; i < tickets.size(); ++i)
        {
            auto status = f.runs.stepStatus(tickets[i]);
            std::printf(
                "%s stop_complete=%d run_state=%u instance_absent=1 ticket=%zu readable=%d\n",
                mode.data(),
                stop.complete(),
                unsigned(info.state),
                i,
                bool(status)
            );
            if (!status)
            {
                const auto* runtime = std::get_if<lux::scene::SceneRuntimeFailure>(&status.error().cause);
                assert(
                    runtime && std::get<lux::scene::ESceneRuntimeError>(runtime->cause) ==
                                   lux::scene::ESceneRuntimeError::INVALID_ID
                );
                std::puts("FAIL late step result: INVALID_ID while Run remains unacknowledged");
                passed = false;
                continue;
            }
            using State = lux::scene::ESceneStepState;
            const auto expected = mode == "r1-failed"            ? State::FAILED
                                  : mode == "r1-mixed" && i == 0 ? State::COMPLETED
                                                                 : State::CANCELLED;
            assert(status->state == expected);
            if (expected == State::COMPLETED)
                assert(status->result);
            else if (expected == State::FAILED)
            {
                assert(!status->result);
                const auto& failure = std::get<lux::scene::SceneDriveFailure>(status->result.error().cause);
                assert(failure.phase == lux::scene::ESceneDrivePhase::PUBLICATION);
                assert(std::any_cast<int>(std::get<lux::scene::SceneExecutionFailure>(failure.cause).cause) == 731);
            }
            else
            {
                assert(
                    !status->result && std::get<lux::scene::ESceneRuntimeError>(status->result.error().cause) ==
                                           lux::scene::ESceneRuntimeError::STOPPED
                );
            }
            std::printf("PASS original terminal state=%u and cause retained\n", unsigned(status->state));
            assert(f.runs.acknowledgeStep(tickets[i]));
            assert(!f.runs.stepStatus(tickets[i]) && !f.runs.acknowledgeStep(tickets[i]));
        }
        assert(f.runs.acknowledgeStop(id));
        for (auto ticket : tickets)
            assert(!f.runs.stepStatus(ticket));
        if (passed && mode == "r1-failed")
        {
            const auto another = f.start();
            assert(f.runs.pause(another));
            f.until([&] { return take(f.runs.info(another)).state == ERunState::PAUSED; });
            const auto instance = take(f.runs.info(another)).instance;
            Probe::custom_failure = true;
            take(f.runtime->borrowInstance(instance)).get().ctx().get<Probe::Facts>().fail = true;
            const auto ticket = take(f.runs.step(another));
            f.until([&] { return take(f.runs.info(another)).state == ERunState::FAILED; });
            assert(!f.runtime->borrowClock(instance));
            unsigned copies{};
            Probe::copying_failure = [&] {
                ++copies;
                const auto removing = f.runs.acknowledgeStop(another);
                assert(!removing && std::get<ERunError>(removing.error().cause) == ERunError::BUSY);
                const auto recursive = f.runs.acknowledgeStep(ticket);
                assert(!recursive && std::get<ERunError>(recursive.error().cause) == ERunError::BUSY);
            };
            const auto value = take(f.runs.stepStatus(ticket));
            Probe::copying_failure = {};
            assert(copies > 0 && value.state == lux::scene::ESceneStepState::FAILED);
            const auto& cause = std::get<lux::scene::SceneDriveFailure>(value.result.error().cause);
            assert(
                std::any_cast<Probe::Failure>(std::get<lux::scene::SceneExecutionFailure>(cause.cause).cause).reason ==
                731
            );
            assert(f.runs.acknowledgeStep(ticket) && f.runs.acknowledgeStop(another));
            Probe::custom_failure = false;
            std::puts("PASS retained error copy cannot recursively acknowledge step or remove Run");
        }
        return passed;
    }
    void callbackResults()
    {
        Fixture f;
        const auto id = f.start();
        assert(f.runs.pause(id));
        f.until([&] { return take(f.runs.info(id)).state == ERunState::PAUSED; });
        const auto instance = take(f.runs.info(id)).instance;
        const auto ticket = take(f.runs.step(id));
        const auto destroyed = Probe::destroyed;
        bool in_flight = true, called = false;
        StopTicket stop;
        Probe::in_flight = &in_flight;
        Probe::maintaining = [&] {
            if (called)
                return;
            called = true;
            stop = take(f.runs.stop(id));
            assert(!stop.complete() && Probe::destroyed == destroyed);
            const auto nested = f.runtime->driveFrame();
            assert(
                !nested &&
                std::get<lux::scene::ESceneRuntimeError>(nested.error().cause) == lux::scene::ESceneRuntimeError::BUSY
            );
        };
        f.frame();
        Probe::maintaining = {};
        assert(called && !stop.complete() && Probe::destroyed == destroyed);
        f.frame();
        assert(!stop.complete() && Probe::destroyed == destroyed);
        in_flight = false;
        f.until([&] { return stop.complete(); });
        Probe::in_flight = nullptr;
        assert(Probe::destroyed == destroyed + 1 && !f.runtime->borrowClock(instance));
        const auto status = take(f.runs.stepStatus(ticket));
        assert(status.state == lux::scene::ESceneStepState::CANCELLED && !status.result);
        assert(
            std::get<lux::scene::ESceneRuntimeError>(status.result.error().cause) ==
            lux::scene::ESceneRuntimeError::STOPPED
        );
        assert(f.runs.acknowledgeStop(id));
        assert(!f.runs.stepStatus(ticket));
        assert(!f.runtime->stepStatus(ticket.step, stop.retirement));
        f.frame();
        assert(Probe::destroyed == destroyed + 1);
        std::puts("PASS R06-R1-04 callback stop, real pending endpoint, outer BUSY, one destruction, late CANCELLED");
    }
    void resultCapacity()
    {
        Fixture f{1};
        const auto destroyed = Probe::destroyed;
        RunId previous;
        lux::scene::SceneInstanceId previous_instance;
        for (unsigned cycle{}; cycle < 64; ++cycle)
        {
            const auto id = f.start();
            assert(f.runs.pause(id));
            f.until([&] { return take(f.runs.info(id)).state == ERunState::PAUSED; });
            const auto instance = take(f.runs.info(id)).instance;
            if (previous.valid())
            {
                assert(id.slot == previous.slot && id.generation != previous.generation);
                assert(instance.slot == previous_instance.slot && instance.generation != previous_instance.generation);
                assert(!f.runs.info(previous));
            }
            std::vector<StepTicket> tickets;
            for (unsigned i{}; i < 32; ++i)
                tickets.push_back(take(f.runs.step(id)));
            assert(!f.runs.step(id));
            assert(!f.runs.acknowledgeStep(tickets.front()));
            const auto stop = take(f.runs.stop(id));
            assert(!f.runs.acknowledgeStop(id));
            f.until([&] { return stop.complete(); });
            assert(Probe::destroyed == destroyed + cycle + 1);
            const auto full = f.controller.prepare(*f.author, f.environment());
            assert(!full && std::get<ERunError>(full.error().cause) == ERunError::CAPACITY);
            for (auto ticket : tickets)
                assert(take(f.runs.stepStatus(ticket)).state == lux::scene::ESceneStepState::CANCELLED);
            assert(f.runs.acknowledgeStep(tickets.front()));
            assert(!f.runs.acknowledgeStep(tickets.front()));
            assert(!f.runtime->stepStatus(tickets.front().step, stop.retirement));
            assert(take(f.runs.stop(id)).complete());
            assert(f.runs.acknowledgeStop(id));
            assert(!f.runs.acknowledgeStop(id));
            for (auto ticket : tickets)
            {
                assert(!f.runs.stepStatus(ticket));
                assert(!f.runtime->stepStatus(ticket.step, stop.retirement));
            }
            previous = id;
            previous_instance = instance;
        }
        assert(Probe::destroyed == destroyed + 64);
        std::puts("PASS R06-R1-05 64 generations, 32 slots, single/aggregate acknowledgement, no heavy retention");
    }
    void modelInsertion()
    {
        Fixture f;
        object::ObjectMessageQueue queue{take(object::ObjectMessageQueue::create(64))};
        project::ProjectCatalogModel catalog{queue.dispatcherRef(), 42};
        const asset::AssetId model_id{uuid("model-insertion")};
        const asset::AssetId mesh_id{uuid("model-mesh")}, material_id{uuid("model-material")};
        const std::vector<AssetCatalogEntry> entries{
            {model_id, model_id, asset::ModelAsset::primary_magic, "Content/test"},
            {mesh_id, model_id, asset::MeshAsset::primary_magic, "Content/test"},
            {material_id, model_id, asset::MaterialAsset::primary_magic, "Content/test"}
        };
        assert(catalog.replace("models", entries));
        auto description = std::make_shared<rdesc::ModelDescription>();
        description->primitives.push_back({mesh_id, material_id});
        description->nodes.resize(3);
        description->nodes[2].primitives.push_back(0);
        description->nodes[0].children.push_back(1);
        description->nodes[1].children.push_back(2);
        auto model = take(asset::ModelAsset::create({model_id, asset::ModelAsset::asset_type}, description));
        auto bytes = std::make_shared<const std::vector<std::byte>>(
            take(asset::TAssetSerDeser<asset::ModelAsset>::encode(*model, asset::AssetEncodeLimits{1024 * 1024}))
        );
        auto port = take(process::asset_loading::makeAssetReadOverlay(
            {{model_id, {cxx::SharedBytes<>::fromOwner(bytes, *bytes)}}},
            {}
        ));
        auto access = f.authors.access<SceneSession>();
        auto key = take(f.authors.key<SceneSession>(f.author_id));
        const auto start = [&] {
            return take(ModelCreationOperation::start(
                f.execution,
                access,
                catalog,
                port,
                f.schemas,
                {key, f.author->describe().current, catalog.reference(model_id), {2, 3, 4}, {0}}
            ));
        };
        auto operation = start();
        const auto initial = take(f.author->capture());
        f.until([&] { return operation->stage() != EModelCreationStage::READING; });
        assert(operation->stage() == EModelCreationStage::READY);
        assert(f.author->describe().current == initial.content());
        const auto guarded = take(f.author->read()).withRead([&](const SceneReadView&) -> SceneEditResult<void> {
            auto rejected = operation->commit();
            assert(!rejected && operation->stage() == EModelCreationStage::READY);
            return {};
        });
        assert(guarded);
        const auto receipt = take(operation->commit());
        assert(operation->stage() == EModelCreationStage::INSERTED);
        assert(take(f.author->capture()).objects().size() == initial.objects().size() + 3);
        assert(take(operation->commit()).content == receipt.content);
        assert(f.author->undo());
        assert(take(f.author->capture()).objects().size() == initial.objects().size());
        assert(f.author->redo());
        auto stale = start();
        f.until([&] { return stale->stage() != EModelCreationStage::READING; });
        assert(f.author->undo());
        const auto before_stale = f.author->describe();
        assert(!stale->commit() && stale->stage() == EModelCreationStage::FAILED);
        assert(f.author->describe().current == before_stale.current);
        assert(f.author->describe().observed == before_stale.observed);
        auto cancelled = start();
        cancelled->cancel();
        assert(!cancelled->commit() && cancelled->stage() == EModelCreationStage::CANCELLED);
        auto catalogue = start();
        f.until([&] { return catalogue->stage() != EModelCreationStage::READING; });
        catalog.setFailure(project::EProjectQueryError::BUSY);
        assert(!catalogue->commit() && catalogue->stage() == EModelCreationStage::READY);
        catalog.setFailure({});
        assert(catalog.replace("new version", entries));
        assert(!catalogue->commit() && catalogue->stage() == EModelCreationStage::FAILED);
        {
            SceneSessionLimits limits;
            limits.history.max_staging_bytes = 4096;
            Fixture bounded{4, limits};
            auto large = std::make_shared<rdesc::ModelDescription>(*description);
            large->nodes.resize(128);
            for (std::uint32_t i = 3; i != large->nodes.size(); ++i)
                large->nodes[0].children.push_back(i);
            auto source = take(asset::ModelAsset::create({model_id, asset::ModelAsset::asset_type}, large));
            auto image = std::make_shared<const std::vector<std::byte>>(
                take(asset::TAssetSerDeser<asset::ModelAsset>::encode(*source, asset::AssetEncodeLimits{1024 * 1024}))
            );
            auto reads = take(process::asset_loading::makeAssetReadOverlay(
                {{model_id, {cxx::SharedBytes<>::fromOwner(image, *image)}}},
                {}
            ));
            auto insertion = take(ModelCreationOperation::start(
                bounded.execution,
                bounded.authors.access<SceneSession>(),
                catalog,
                reads,
                bounded.schemas,
                {take(bounded.authors.key<SceneSession>(bounded.author_id)),
                 bounded.author->describe().current,
                 catalog.reference(model_id),
                 {0, 0, 0},
                 {0}}
            ));
            bounded.until([&] { return insertion->stage() != EModelCreationStage::READING; });
            const auto before = bounded.author->describe();
            const auto objects = take(bounded.author->capture()).objects().size();
            assert(!insertion->commit() && insertion->stage() == EModelCreationStage::FAILED);
            assert(bounded.author->describe().current == before.current);
            assert(bounded.author->describe().observed == before.observed);
            assert(take(bounded.author->capture()).objects().size() == objects);
        }
        auto closed = start();
        f.until([&] { return closed->stage() != EModelCreationStage::READING; });
        auto permit = take(f.authors.prepareClose(f.author->describe().current));
        assert(f.authors.close(permit));
        f.author = nullptr;
        assert(!closed->commit() && closed->stage() == EModelCreationStage::FAILED);
        assert(f.authors.size() == 0);
        cancelled.reset(); // The accepted read result is still drained even with no view/session.
        std::puts("PASS X12-07 real model decode, one batch/undo, BUSY retry, stale/cancel/catalog/closed no insertion"
        );
    }
    void completion()
    {
        Fixture f;
        auto first = take(f.controller.prepare(*f.author, f.environment()));
        f.until([&] { return first->ready(); });
        auto second = take(f.controller.prepare(*f.author, f.environment()));
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (true)
        {
            const auto tasks = f.execution.taskInfos();
            if (!tasks.empty() &&
                std::ranges::all_of(tasks, [](const auto& task) { return task.finished.has_value(); }))
                break;
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        assert(!second->ready());
        unsigned calls{};
        Probe::installing = [&] {
            ++calls;
            assert(f.execution.collectCompletions() && f.execution.dispatchTaskEvents());
            assert(second->ready());
            const auto nested = f.controller.adopt(*second);
            assert(!nested && std::get<ERunError>(nested.error().cause) == ERunError::BUSY);
            const auto maintained = f.runs.update();
            assert(!maintained && std::get<ERunError>(maintained.error().cause) == ERunError::BUSY);
        };
        const auto a = take(f.controller.adopt(*first));
        Probe::installing = {};
        assert(calls == 1);
        const auto b = take(f.controller.adopt(*second));
        assert(a != b && !f.controller.adopt(*second));
        // Pause does not prevent accepted partition reads/completions from becoming resident.
        assert(f.runs.pause(b));
        f.until([&] { return f.entity(b) != ecs::NullEntity; });
        assert(take(f.runs.info(b)).progress.time.step_index == 0);
        f.stop(a);
        f.stop(b);
        auto abandoned = take(f.controller.prepare(*f.author, f.environment()));
        abandoned.reset();
        f.until([&] { return f.execution.taskInfos().empty(); });
        std::puts("PASS X06-02/04 accepted completion during install, outer guard retained, once-only adoption, pause "
                  "receives reads, abandoned preparation drains");
    }
}
int main(int argc, char** argv)
{
    const std::string_view mode = argc > 1 ? argv[1] : "isolation";
    if (mode == "model-insertion")
        modelInsertion();
    else if (mode == "isolation")
        isolation();
    else if (mode == "controls")
        controls();
    else if (mode == "failure")
        failure();
    else if (mode == "completion")
        completion();
    else if (mode == "r1-callback")
        callbackResults();
    else if (mode == "r1-capacity")
        resultCapacity();
    else if (mode == "r1-queued" || mode == "r1-mixed" || mode == "r1-failed")
        return lateResults(mode) ? 0 : 1;
    else
        return 2;
}
