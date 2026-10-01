#include <lux/engine/editor/scene/SceneInteraction.hpp>
#include <lux/engine/editor/scene/RunController.hpp>
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

        explicit Fixture(std::size_t capacity = 4) : runs(*runtime, execution, capacity)
        {
            std::vector<ecs::ComponentSchema> types;
            for (auto group : {ecs::transformComponentSchemas(), ecs::hierarchyComponentSchemas()})
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
                std::move(source)
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
    void selection()
    {
        Fixture f(1);
        auto inspect = f.runs.inspect();
        auto key = take(f.authors.key<SceneSession>(f.author_id));
        SceneInteractionGroup group(f.authors.access<SceneSession>(), key, {5}, inspect);
        auto first = f.start();
        const auto ref = take(inspect.reference(first, f.entity(first)));
        const SceneObjectRef author{f.author_id, f.author->describe().current.state.history, f.object};
        assert(group.select({{author, ref}}));
        assert(group.selection().objects.size() == 2);
        assert(std::holds_alternative<SceneObjectRef>(group.selection().objects[0]));
        assert(std::holds_alternative<RunningObjectRef>(group.selection().objects[1]));
        const auto author_before = f.author->describe();
        f.stop(first);
        assert(group.synchronize() && group.selection().objects.size() == 1);
        const auto second = f.start();
        const auto second_ref = take(inspect.reference(second, f.entity(second)));
        assert(first.slot == second.slot && first.generation != second.generation);
        assert(second_ref.entity == ref.entity); // Same actual EnTT number in distinct real instances.
        assert(!group.select({{ref}}));
        assert(group.select({{author, second_ref}}));
        assert(f.author->describe().current == author_before.current);
        assert(f.author->describe().dirty == author_before.dirty);
        f.stop(second);
        assert(group.synchronize());
        auto permit = take(f.authors.prepareClose(f.author->describe().current));
        assert(f.authors.close(permit));
        assert(group.synchronize() && group.selection().objects.empty());
        assert(!group.select({{author}}));
    }
}
int main()
{
    selection();
    std::puts("PASS X08-02 real author/Run sources, overlapping Entity and reused Run slot");
}
