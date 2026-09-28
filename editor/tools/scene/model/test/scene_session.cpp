#include <lux/engine/editor/scene/SceneSessionAccess.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/scene/CameraSchema.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include "../src/PreparedSceneReload.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::scene;
    namespace ecs = simulation::ecs;

    template <class Result> auto take(Result result)
    {
        if (!result)
        {
            if constexpr (requires { result.error().code; })
                std::fprintf(stderr, "Unexpected error code %u\n", static_cast<unsigned>(result.error().code));
            std::abort();
        }
        return std::move(*result);
    }
    uuids::uuid uuid(std::string_view key)
    {
        return uuids::uuid_name_generator(*uuids::uuid::from_string("01234567-89ab-cdef-0123-456789abcdef"))(key);
    }
    world::WorldObjectId object(std::string_view key)
    {
        return {uuid(key)};
    }
    ecs::ComponentSchemaSet schemas()
    {
        std::vector<ecs::ComponentSchema> values;
        for (auto list :
             {ecs::transformComponentSchemas(),
              ecs::hierarchyComponentSchemas(),
              ecs::visualComponentSchemas(),
              lux::scene::cameraComponentSchemas()})
            for (auto schema : list)
                if (schema.snapshot == ecs::EComponentSnapshotPolicy::COPY)
                    values.push_back(std::move(schema));
        return take(ecs::ComponentSchemaSet::build(std::move(values)));
    }
    lux::scene::ScenePackage package(const ecs::ComponentSchemaSet& schemas, std::string_view root = "root")
    {
        std::vector<world::WorldDataSchemaId> ids;
        for (const auto& schema : schemas.all())
            ids.push_back(world::worldDataSchemaId(schema.id.name));
        ids.push_back(world::worldDataSchemaId("test.unknown"));
        auto simulation = take(std::move(simulation::SimulationDescriptionBuilder{}).build());
        auto description = take(std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved());
        return take(lux::scene::createScenePackage(
            asset::AssetId{uuid(root)},
            "CPU author scene",
            ids,
            std::make_shared<const simulation::SimulationDescription>(std::move(simulation)),
            description
        ));
    }
    struct Fixture final
    {
        ecs::ComponentSchemaSet metadata{schemas()};
        lux::scene::ScenePackage input{package(metadata)};
        sessions::SessionStore store{8};
        SceneSession* session{};
        sessions::SessionId id;

        explicit Fixture(SceneSessionLimits limits = {}, std::string_view root = "root")
            : input(package(metadata, root))
        {
            auto reservation = take(store.reserve<SceneSession>({"lux.editor.scene"}, contracts::CodeLease::builtin()));
            id = reservation.id();
            auto source = take(SceneSource::create(input, metadata));
            auto candidate = take(SceneSession::create(
                id,
                sessions::BoundSource{input.scene->id(), "scene.lux"},
                std::move(source),
                limits
            ));
            session = candidate.get();
            assert(store.prepare(reservation, candidate));
            assert(store.publish(reservation));
        }
        SceneObjectRef ref(world::WorldObjectId id) const
        {
            return {this->id, session->describe().current.state.history, id};
        }
        SceneEditBatch batch(std::string label = "edit") const
        {
            return {session->describe().current, std::move(label), {}};
        }
        SceneObjectData spatial(std::string_view name) const
        {
            ecs::WorldEntityMap identities;
            return {object(name), {0}, {take(encodeSceneValue(ecs::Transform3D{}, metadata, identities, 4096))}};
        }
        void create(std::string_view name)
        {
            auto edit = batch("create");
            edit.edits.push_back(SceneCreateObject{spatial(name)});
            assert(session->apply(std::move(edit)));
        }
        SceneSetField translation(world::WorldObjectId id, double x) const
        {
            return SceneSetField::make<ecs::Transform3D>(
                {ref(id), ecs::componentSchemaId("lux.ecs.Transform3D"), "translation"},
                Eigen::Vector3d{x, 2, 3}
            );
        }
    };

    void content()
    {
        Fixture baseline;
        const auto loaded_state = baseline.session->describe();
        baseline.create("undo-baseline");
        assert(baseline.session->describe().dirty);
        assert(baseline.session->undo());
        assert(!baseline.session->describe().dirty && baseline.session->describe().current == loaded_state.current);
        assert(baseline.session->redo() && baseline.session->describe().dirty);
        auto incomplete = package(baseline.metadata);
        incomplete.partitions.clear();
        assert(!SceneSource::create(incomplete, baseline.metadata));
        auto bytes = std::make_shared<std::vector<std::byte>>(4, std::byte{7});
        auto opaque_package = package(baseline.metadata);
        opaque_package.package.mount_hint = "author";
        opaque_package.package.entries.push_back({{}, lux::cxx::SharedBytes<>::fromOwner(bytes, *bytes)});
        auto detached = take(SceneSource::create(opaque_package, baseline.metadata));
        (*bytes)[0] = std::byte{9};
        auto opaque_session = take(SceneSession::create({99, 0, 1}, {}, std::move(detached)));
        auto opaque_snapshot = take(opaque_session->capture());
        assert(opaque_snapshot.package().entries.back().bytes.view()[0] == std::byte{7});
        assert(opaque_snapshot.package().mount_hint == "author");
        opaque_session.reset();
        assert(opaque_snapshot.package().entries.back().bytes.view()[0] == std::byte{7});
        Fixture f;
        assert(!f.session->describe().dirty);
        const auto start = f.session->describe();
        f.create("a");
        f.create("b");
        auto edit = f.batch("fields and parent");
        edit.edits.push_back(f.translation(object("a"), 3));
        edit.edits.push_back(SceneReparentObject{f.ref(object("b")), object("a")});
        assert(f.session->apply(std::move(edit)));
        auto read = take(f.session->read());
        assert(take(read.parent(f.ref(object("b")))) == object("a"));
        const auto changed = take(read.component(f.ref(object("a")), ecs::componentSchemaId("lux.ecs.Transform3D")));
        assert(f.session->undo());
        assert(take(f.session->read()).parent(f.ref(object("b")))->valid() == false);
        assert(f.session->redo());
        assert(take(take(f.session->read()).component(f.ref(object("a")), changed.schema)) == changed);
        const auto frozen = take(f.session->capture());
        auto field = f.batch();
        field.edits.push_back(f.translation(object("a"), 9));
        assert(f.session->apply(std::move(field)));
        assert(
            frozen.objects()[0].components[0].bytes !=
            take(take(f.session->read()).component(f.ref(object("a")), changed.schema)).bytes
        );
        assert(f.session->undo());
        const auto before_noop = f.session->describe();
        auto noop = f.batch();
        noop.edits.push_back(f.translation(object("a"), 3));
        auto applied = take(f.session->apply(std::move(noop)));
        assert(applied.effect == editing::EEditEffect::NO_CHANGE);
        assert(f.session->describe().current == before_noop.current);
        assert(f.session->describe().observed == before_noop.observed);
        assert(!f.session->capture({1}));

        auto camera =
            take(makeSceneCameraObject(object("camera"), {0}, lux::scene::Camera{}, ecs::Transform3D{}, f.metadata));
        auto cameras = f.batch("camera");
        cameras.edits.push_back(SceneCreateObject{std::move(camera)});
        assert(f.session->apply(std::move(cameras)));
        auto model = std::make_shared<rdesc::ModelDescription>();
        model->primitives.push_back({asset::AssetId{uuid("mesh")}, asset::AssetId{uuid("material")}});
        model->nodes.push_back({});
        model->nodes.front().primitives.push_back(0);
        auto loaded =
            take(asset::ModelAsset::create({asset::AssetId{uuid("model")}, asset::ModelAsset::asset_type}, model));
        auto nodes =
            take(makeSceneModelObjects(*loaded, Eigen::Vector3d{1, 0, 0}, {0}, f.input.world->data(), f.metadata));
        auto insertion = f.batch("model");
        for (auto& node : nodes)
            insertion.edits.push_back(SceneCreateObject{std::move(node)});
        assert(f.session->apply(std::move(insertion)));
        assert(f.session->undo() && f.session->redo());
        auto add_component = f.batch("add camera component");
        ecs::WorldEntityMap identities;
        add_component.edits.push_back(SceneAddComponent{
            f.ref(object("a")),
            take(encodeSceneValue(lux::scene::Camera{}, f.metadata, identities, 4096))
        });
        assert(f.session->apply(std::move(add_component)));
        auto remove_component = f.batch("remove camera component");
        remove_component.edits.push_back(
            SceneRemoveComponent{{f.ref(object("a")), ecs::componentSchemaId("lux.scene.Camera"), {}}}
        );
        assert(f.session->apply(std::move(remove_component)));
        assert(!take(f.session->read()).component(f.ref(object("a")), ecs::componentSchemaId("lux.scene.Camera")));
        assert(f.session->undo());
        assert(take(f.session->read()).component(f.ref(object("a")), ecs::componentSchemaId("lux.scene.Camera")));
        assert(f.session->redo());

        auto dangling = f.batch("must preserve incoming references");
        dangling.edits.push_back(SceneEraseObject{f.ref(object("a"))});
        const auto before_delete = f.session->describe();
        assert(!f.session->apply(std::move(dangling)));
        assert(f.session->describe().current == before_delete.current);
        auto erase = f.batch("explicit deletion set");
        erase.edits.push_back(SceneEraseObject{f.ref(object("a"))});
        erase.edits.push_back(SceneEraseObject{f.ref(object("b"))});
        assert(f.session->apply(std::move(erase)));
        assert(!take(f.session->read()).contains(f.ref(object("a"))));
        assert(f.session->undo());
        assert(take(take(f.session->read()).parent(f.ref(object("b")))) == object("a"));

        auto configuration = take(f.session->read()).configuration();
        lux::scene::SceneDescriptionBuilder builder;
        builder.setWorld(configuration.world->id());
        builder.setSimulation(configuration.simulation->id());
        const std::array payload{std::byte{7}, std::byte{9}};
        assert(builder.addSystem(
            {44},
            "cpu author setting",
            system::systemTypeId("test.cpu"),
            1,
            "test.cpu.configuration",
            2,
            payload
        ));
        auto configured = take(std::move(builder).build());
        configuration.scene = take(lux::scene::SceneAsset::create(
            configuration.scene->info(),
            std::make_shared<const lux::scene::SceneDescription>(std::move(configured))
        ));
        auto settings = f.batch("scene configuration");
        settings.edits.push_back(SceneSetConfiguration{configuration});
        assert(f.session->apply(std::move(settings)));
        assert(take(f.session->capture()).configuration().scene->data().systemCount() == 1);
        assert(f.session->undo());
        assert(take(f.session->read()).configuration().scene->data().systemCount() == 0);
        assert(f.session->redo());

        const auto stable = f.session->describe();
        auto same = f.batch();
        same.edits.push_back(SceneSetConfiguration{take(f.session->read()).configuration()});
        assert(take(f.session->apply(std::move(same))).effect == editing::EEditEffect::NO_CHANGE);
        assert(f.session->describe().current == stable.current);

        auto opaque = f.batch("unknown current-format payload");
        const SceneComponentData unknown{
            ecs::componentSchemaId("test.unknown"),
            31,
            {std::byte{0x55}, std::byte{0xaa}}
        };
        opaque.edits.push_back(SceneCreateObject{{object("opaque"), {0}, {unknown}}});
        assert(f.session->apply(std::move(opaque)));
        auto field_after_unknown = f.batch();
        field_after_unknown.edits.push_back(f.translation(object("a"), 14));
        assert(f.session->apply(std::move(field_after_unknown)));
        assert(take(take(f.session->read()).component(f.ref(object("opaque")), unknown.schema)) == unknown);
        assert(f.session->undo() && f.session->redo());
        const auto opaque_frozen = take(f.session->capture());
        auto opaque_row = std::ranges::find(opaque_frozen.objects(), object("opaque"), &SceneObjectData::id);
        assert(opaque_row != opaque_frozen.objects().end() && opaque_row->components.front() == unknown);
        auto unsafe = f.batch();
        unsafe.edits.push_back(SceneEraseObject{f.ref(object("a"))});
        auto refused = f.session->apply(std::move(unsafe));
        assert(!refused && refused.error().code == ESceneEditError::UNKNOWN_REFERENCE);
        assert(f.session->describe().current != start.current);
        std::puts(
            "X02-01/Q01/Q09/Q22 real CPU structure/field/configuration/camera/model/undo/redo/unknown capture PASS"
        );
    }

    void atomic()
    {
        SceneSessionLimits limits;
        limits.history.max_staging_bytes = 8192;
        Fixture f{limits};
        f.create("a");
        f.create("b");
        const auto baseline = f.session->describe();
        const auto frozen = take(f.session->capture());
        const auto unchanged = [&] {
            assert(f.session->describe().current == baseline.current);
            assert(f.session->describe().observed == baseline.observed);
            assert(f.session->describe().dirty == baseline.dirty);
            const auto snapshot = take(f.session->capture());
            assert(std::ranges::equal(snapshot.objects(), frozen.objects()));
        };
        auto third = f.batch();
        third.edits.push_back(SceneCreateObject{f.spatial("first")});
        third.edits.push_back(SceneCreateObject{f.spatial("second")});
        auto large = f.spatial("third");
        large.components.front().bytes.resize(8192);
        third.edits.push_back(SceneCreateObject{std::move(large)});
        auto failure = f.session->apply(std::move(third));
        assert(
            !failure && failure.error().code == ESceneEditError::BUDGET && failure.error().object == object("third")
        );
        unchanged();
        auto cycle = f.batch();
        cycle.edits.push_back(SceneReparentObject{f.ref(object("a")), object("b")});
        cycle.edits.push_back(SceneReparentObject{f.ref(object("b")), object("a")});
        assert(!f.session->apply(std::move(cycle)));
        unchanged();
        auto missing = f.batch();
        auto invalid = f.spatial("invalid");
        invalid.components.front().schema = ecs::componentSchemaId("missing.schema");
        missing.edits.push_back(SceneCreateObject{std::move(invalid)});
        assert(!f.session->apply(std::move(missing)));
        unchanged();
        auto bad_field = f.batch();
        bad_field.edits.push_back(SceneSetField::make<ecs::Transform3D>(
            {f.ref(object("a")), ecs::componentSchemaId("lux.ecs.Transform3D"), "translation"},
            2
        ));
        assert(!f.session->apply(std::move(bad_field)));
        unchanged();
        SceneSessionLimits small_history;
        small_history.history.max_retained_bytes = 1;
        Fixture retained{small_history};
        const auto initial = retained.session->describe();
        auto over = retained.batch();
        over.edits.push_back(SceneCreateObject{retained.spatial("over-limit")});
        assert(!retained.session->apply(std::move(over)));
        assert(retained.session->describe().current == initial.current);
        assert(take(retained.session->read()).objects().empty());
        std::puts(
            "X02-02/Q09 third-object budget, cycle, schema, field rejection: source/history/baseline unchanged PASS"
        );
    }

    void identity()
    {
        Fixture a, b{{}, "different-root"};
        a.create("same");
        b.create("same");
        const auto other_package = package(a.metadata, "different-root");
        assert(other_package.scene->id() != a.input.scene->id());
        assert(!detail::PreparedSceneReload::prepare(*a.session, take(SceneSource::create(other_package, a.metadata))));
        auto source = take(SceneSource::create(a.input, a.metadata));
        auto reload = take(detail::PreparedSceneReload::prepare(*a.session, std::move(source)));
        const auto old = a.ref(object("same"));
        auto wrong = b.batch();
        wrong.edits.push_back(SceneEraseObject{old});
        assert(!b.session->apply(std::move(wrong)));
        assert(reload.adopt(*a.session));
        auto stale = a.batch();
        stale.edits.push_back(SceneEraseObject{old});
        assert(!a.session->apply(std::move(stale)));
        a.create("same");
        assert(take(a.session->read()).objects().front().object == old.object);
        assert(!take(a.session->read()).contains(old));
        std::puts("X02-04/Q06/Q26 stable author UUID across roots; wrong session/history rejected PASS");
    }

    void changes()
    {
        SceneSessionLimits limits;
        limits.change_records = 2;
        Fixture f{limits};
        const auto initial = take(f.session->capture()).cursor();
        f.create("a");
        auto recent = take(f.session->capture()).cursor();
        f.create("b");
        f.create("c");
        assert(take(f.session->changesSince(initial)).status == ESceneChanges::RESET_REQUIRED);
        assert(take(f.session->changesSince(recent)).objects.size() == 2);
        auto source = take(SceneSource::create(f.input, f.metadata));
        auto reload = take(detail::PreparedSceneReload::prepare(*f.session, std::move(source)));
        assert(reload.adopt(*f.session));
        assert(take(f.session->changesSince(recent)).status == ESceneChanges::RESET_REQUIRED);
        assert(!f.session->describe().dirty);
        const auto baseline = f.session->describe();
        auto prepared =
            take(detail::PreparedSceneReload::prepare(*f.session, take(SceneSource::create(f.input, f.metadata))));
        f.create("d");
        assert(!prepared.adopt(*f.session));
        assert(f.session->describe().current != baseline.current);
        auto permit = take(f.store.prepareClose(f.session->describe().current));
        assert(!f.session->capture());
        assert(f.store.close(permit));
        SceneSessionLimits small;
        small.change_bytes = 1;
        Fixture overflow{small};
        const auto cursor = take(overflow.session->capture()).cursor();
        overflow.create("large-notice");
        assert(take(overflow.session->changesSince(cursor)).status == ESceneChanges::RESET_REQUIRED);
        std::puts("X02-05/Q06 clipped log and genuine reload HistoryId require reset; stale candidate rejected PASS");
    }
}

void pluginSnapshot();

int main(int argc, char** argv)
{
    const std::string_view test = argc > 1 ? argv[1] : "content";
    if (test == "content")
        content();
    else if (test == "atomic")
        atomic();
    else if (test == "identity")
        identity();
    else if (test == "changes")
        changes();
    else if (test == "plugin")
        pluginSnapshot();
    else
        return 2;
}
