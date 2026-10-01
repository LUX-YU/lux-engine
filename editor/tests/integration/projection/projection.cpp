#include <lux/engine/editor/scene/SceneProjection.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
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

    struct Fixture final
    {
        process::ExecutionRuntime execution{take(process::ExecutionRuntime::create({1, 64, 64, {32}, {}, 0}))};
        std::unique_ptr<lux::scene::SceneRuntime> runtime{take(lux::scene::SceneRuntime::create(execution, {0, 1024}))};
        ScenePresentationHub hub{*runtime, execution, 2};
        sessions::SessionStore authors{4};
        ecs::ComponentSchemaSet schemas;
        std::shared_ptr<const simulation::SimulationSystemRegistry> systems{
            std::make_shared<simulation::SimulationSystemRegistry>()
        };
        std::vector<lux::scene::SceneSystemRegistration> registrations{
            lux::scene::worldLoadingSystemRegistration(),
            lux::scene::transformSystemRegistration()
        };
        SceneSession* author{};
        sessions::SessionId author_id;
        world::WorldObjectId object{uuid("object")};
        Fixture()
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
                SceneSessionLimits{.change_records = 1}
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
        ProjectionEnvironment environment() const
        {
            return {schemas, systems, registrations};
        }
        void frame()
        {
            assert(execution.collectCompletions());
            assert(runtime->driveFrame());
            hub.collectReleased();
        }
        void set(double x)
        {
            const auto before = author->describe();
            SceneEditBatch batch{before.current, "move", {}};
            batch.edits.push_back(SceneSetField::make<ecs::Transform3D>(
                {{author_id, before.current.state.history, object},
                 ecs::componentSchemaId("lux.ecs.Transform3D"),
                 "translation"},
                Eigen::Vector3d{x, 0, 0}
            ));
            assert(author->apply(std::move(batch)));
        }
    };
}
int main()
{
    const auto begin = std::chrono::steady_clock::now();
    Fixture f;
    auto first = take(f.hub.acquire(*f.author, f.environment()));
    auto second = take(f.hub.acquire(*f.author, f.environment()));
    assert(first == second && first->instance() == second->instance() && f.hub.size() == 1);
    const auto initial = first->version();
    const auto old = first->instance();
    f.frame();
    const auto time =
        std::visit([](const auto& clock) { return clock.snapshot(); }, take(f.runtime->borrowClock(old)).get());
    assert(time.step_index == 0 && time.elapsed.count() == 0);
    auto registry = take(f.runtime->borrowInstance(old));
    auto entity = registry.get().ctx().get<lux::scene::WorldResidency>().identities().entity(f.object);
    assert(entity != ecs::NullEntity && registry.get().get<ecs::Transform3D>(entity).translation.x() == 10);
    f.set(20);
    f.set(30); // Bounded source ledger lost the original cursor.
    const auto author = f.author->describe();
    assert(first->update(*f.author));
    assert(first->version().content == author.current && first->rebuildCount() == 2);
    assert(first->instance() != old && second->instance() == first->instance());
    f.frame();
    assert(!f.runtime->borrowInstance(old));
    registry = take(f.runtime->borrowInstance(first->instance()));
    entity = registry.get().ctx().get<lux::scene::WorldResidency>().identities().entity(f.object);
    assert(entity != ecs::NullEntity && registry.get().get<ecs::Transform3D>(entity).translation.x() == 30);
    assert(first->update(*f.author) && first->rebuildCount() == 2);
    assert(f.author->describe().current == author.current && f.author->describe().dirty == author.dirty);
    first.reset();
    f.hub.collectReleased();
    assert(f.hub.size() == 1);
    second.reset();
    f.hub.collectReleased();
    assert(f.hub.size() == 1);
    f.frame();
    assert(f.hub.size() == 0);
    for (unsigned iteration = 0; iteration < 32; ++iteration)
    {
        auto a = take(f.hub.acquire(*f.author, f.environment(), 1));
        auto b = take(f.hub.acquire(*f.author, f.environment(), 2));
        auto full = f.hub.acquire(*f.author, f.environment(), 3);
        assert(!full && std::get<EProjectionError>(full.error().cause) == EProjectionError::CAPACITY);
        a.reset();
        b.reset();
        f.hub.collectReleased();
        f.frame();
        assert(f.hub.size() == 0);
    }
    std::puts(
        "X07-07 actual CPU scene: shared instance, RESET_REQUIRED rebuild, correct values, bounded retirement recovery"
    );
}
