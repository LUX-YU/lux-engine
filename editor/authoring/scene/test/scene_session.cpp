#include "../src/SceneSessionData.hpp"
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/scene/CameraSchema.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <lux/engine/editor/scene/PreparedSceneReload.hpp>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <chrono>

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
            auto reservation = take(store.reserve<SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin()));
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

    void snapshotCost()
    {
        for (const std::size_t count : {1000u, 10000u})
        {
            detail::SceneSourceAccess::Data source;
            source.schemas = schemas();
            source.objects.reserve(count);
            source.identities.reserve(count);
            for (std::size_t i = 0; i != count; ++i)
            {
                const auto id = object(std::to_string(i));
                const auto entity = source.registry.create();
                assert(source.identities.bind(id, entity));
                source.registry.emplace<ecs::Transform3D>(entity);
                if (i % 4 == 0)
                    source.registry.emplace<ecs::Parent>(entity);
                source.objects.push_back({id, {0}, {}});
            }
            auto bytes = std::make_shared<const std::vector<std::byte>>(8 * 1024 * 1024, std::byte{42});
            source.volumes.push_back(cxx::SharedBytes<>::fromOwner(bytes, *bytes));
            Fixture frozen_source;
            auto& frozen = detail::SceneSessionAccess::data(*frozen_source.session).source;
            detail::SceneSourceAccess::data(frozen).volumes.clear();
            detail::SceneSourceAccess::data(frozen).package = {};
            detail::SceneSourceAccess::data(frozen).partition_ids.clear();
            detail::SceneBudget admission_budget{256 * 1024 * 1024};
            assert(detail::SceneSourceAccess::copyOpaque(source, detail::SceneSourceAccess::data(frozen), admission_budget));
            for (int sample = 0; sample != 7; ++sample)
            {
                detail::SceneBudget budget{256 * 1024 * 1024};
                const auto start = std::chrono::steady_clock::now();
                auto values = take(detail::SceneSourceAccess::objects(source, budget));
                const auto encoded = std::chrono::steady_clock::now();
                detail::SceneSourceAccess::Data opaque;
                assert(detail::SceneSourceAccess::retainOpaque(frozen, opaque, budget));
                const auto copied = std::chrono::steady_clock::now();
                std::size_t components{}, encoded_bytes{};
                for (const auto& value : values)
                    for (const auto& component : value.components)
                    {
                        ++components;
                        encoded_bytes += component.bytes.size();
                    }
                assert(components == count + count / 4);
                assert(opaque.volumes[0].view()[0] == std::byte{42});
                std::printf("EC1 snapshot sample=%d warmup=%d n=%zu schemas=%zu components=%zu encoded_bytes=%zu "
                            "objects_us=%.3f opaque_us=%.3f opaque_copied_bytes=%zu logical_budget=%zu\n",
                    sample, sample < 2, count, source.schemas.all().size(), components, encoded_bytes,
                    std::chrono::duration<double, std::micro>(encoded - start).count(),
                    std::chrono::duration<double, std::micro>(copied - encoded).count(),
                    opaque.volumes[0].view().data() == detail::SceneSourceAccess::data(frozen).volumes[0].view().data() ? 0 : bytes->size(), budget.used());
            }
        }
    }
    void applicability()
    {
        const auto metadata = schemas();
        const std::string_view t2[]{"lux.ecs.Transform2D"};
        const std::string_view t3[]{"lux.ecs.Transform3D"};
        const std::string_view hierarchy[]{"lux.ecs.Parent"};
        const std::string_view unknown[]{"test.removed.plugin"};
        for (unsigned mask = 0; mask < 4; ++mask)
        {
            std::vector<world::WorldDataSchemaId> ids{world::worldDataSchemaId(unknown[0])};
            if (mask & 1) ids.push_back(world::worldDataSchemaId(t2[0]));
            if (mask & 2) ids.push_back(world::worldDataSchemaId(t3[0]));
            auto simulation = take(std::move(simulation::SimulationDescriptionBuilder{}).build());
            auto description = take(std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved());
            auto input = take(lux::scene::createScenePackage(asset::AssetId{uuid("applicability")}, "facts", ids,
                std::make_shared<const simulation::SimulationDescription>(std::move(simulation)), description));
            sessions::SessionStore store{8};
            auto reservation = take(store.reserve<SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin()));
            auto candidate = take(SceneSession::create(reservation.id(), {}, take(SceneSource::create(input, metadata))));
            auto* session = candidate.get();
            assert(store.prepare(reservation, candidate) && store.publish(reservation));
            const auto initial = session->describe();
            auto read = take(session->read());
            assert(read.withRead([&](const SceneReadView& source) -> SceneEditResult<void> {
                auto facts = source.facts();
                auto two = queryApplicability(facts, {t2, true, true});
                auto three = queryApplicability(facts, {t3, true, true});
                assert(two.supported() == bool(mask & 1) && three.supported() == bool(mask & 2));
                assert(two.based_on == initial.current && three.based_on == initial.current);
                assert(queryApplicability(facts, {hierarchy}).reason == EApplicabilityReason::UNDECLARED_SCHEMA);
                assert(queryApplicability(facts, {unknown}).reason == EApplicabilityReason::MISSING_PROVIDER);
                assert(queryApplicability(facts, {{}, false, false, "lux.spatial.builtin.grid2d", 1}).reason ==
                       EApplicabilityReason::PARTITION_MISMATCH);
                facts.available = false;
                assert(queryApplicability(facts, {}).status == EApplicability::TEMPORARILY_UNAVAILABLE);
                assert(queryApplicability(facts, {hierarchy}).status == EApplicability::NOT_APPLICABLE);
                SceneEditBatch blocked{initial.current, "nested", {}};
                blocked.edits.push_back(SceneCreateObject{{object("blocked"), {0}, {}}});
                auto refused = session->apply(std::move(blocked));
                assert(!refused && refused.error().session == sessions::ESessionError::BUSY);
                return {};
            }));
            assert(session->describe().current == initial.current && session->describe().dirty == initial.dirty);
            auto object3 = take(makeSceneObject(object("three"), {0}, EObjectSpace::SPACE_3D, false, metadata));
            SceneEditBatch create_three{initial.current, "create three", {}};
            create_three.edits.push_back(SceneCreateObject{std::move(object3)});
            auto inserted = session->apply(std::move(create_three));
            assert(bool(inserted) == bool(mask & 2));
            if (inserted)
            {
                assert(session->undo());
                assert(session->describe().current == initial.current);
            }
            SceneObjectData opaque{object("opaque"), {0}, {{ecs::componentSchemaId(unknown[0]), 1, {std::byte{42}}}}};
            SceneEditBatch create_opaque{session->describe().current, "preserve unknown", {}};
            create_opaque.edits.push_back(SceneCreateObject{opaque});
            assert(session->apply(std::move(create_opaque)));
            auto frozen = take(session->capture());
            assert(frozen.objects().back().components == opaque.components);
            auto reopened = take(SceneSource::create(take(buildSceneSnapshotPackage(frozen)), metadata));
            auto rebuilt = take(SceneSession::create({999, 0, 1}, {}, std::move(reopened)));
            assert(take(rebuilt->capture()).objects().back().components == opaque.components);
            const auto changed = session->describe().current;
            assert(changed != initial.current);
            assert(take(session->read()).withRead([&](const SceneReadView& source) -> SceneEditResult<void> {
                assert(queryApplicability(source.facts(), {}).based_on == changed);
                return {};
            }));
        }
        std::puts("EC1: real single World 2D/3D/mixed, no Parent, installed-not-declared, absent provider, gate and opaque PASS");
    }
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
        auto second_snapshot = take(opaque_session->capture());
        assert(second_snapshot.package().entries.back().bytes.view().data() ==
               opaque_snapshot.package().entries.back().bytes.view().data());
        assert(second_snapshot.retainedBytes() == opaque_snapshot.retainedBytes());
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

    void mixedComponents(int scenario)
    {
        Fixture f;
        f.create("mixed");
        f.create("other");
        const auto id = object("mixed");
        const auto schema = ecs::componentSchemaId("lux.ecs.Transform3D");
        const auto before = take(f.session->capture());
        const auto baseline = f.session->describe();
        const auto history_before = take(detail::SceneSessionAccess::data(*f.session).history->view()).snapshot;
        ecs::Transform3D replacement;
        replacement.translation = Eigen::Vector3d{8, 9, 10};
        replacement.scale = Eigen::Vector3d{2, 3, 4};
        const auto encoded = take(encodeSceneValue(replacement, f.metadata, ecs::WorldEntityMap{}, 4096));
        auto batch = f.batch("ordered component lifetime");
        batch.edits.push_back(f.translation(object("other"), 42));
        batch.edits.push_back(f.translation(id, 1));
        batch.edits.push_back(SceneRemoveComponent{{f.ref(id), schema, {}}});
        if (scenario != 2 && scenario != 4)
            batch.edits.push_back(SceneAddComponent{f.ref(id), encoded});
        if (scenario == 3 || scenario == 4)
        {
            batch.edits.push_back(
                SceneSetField::make<ecs::Transform3D>({f.ref(id), schema, "scale"}, Eigen::Vector3d{3, 4, 5})
            );
            batch.edits.push_back(
                SceneSetField::make<ecs::Transform3D>({f.ref(id), schema, "scale"}, Eigen::Vector3d{5, 6, 7})
            );
            replacement.scale = Eigen::Vector3d{5, 6, 7};
        }
        auto result = f.session->apply(std::move(batch));
        std::printf(
            "R02-0%d apply=%d error=%u\n",
            scenario,
            bool(result),
            result ? 0u : static_cast<unsigned>(result.error().code)
        );
        std::fflush(stdout);
        if (scenario == 4)
        {
            assert(!result && result.error().code == ESceneEditError::INVALID_COMPONENT);
            const auto after = take(f.session->capture());
            assert(std::ranges::equal(after.objects(), before.objects()));
            assert(after.cursor() == before.cursor());
            const auto equal_asset = []<class T>(const std::shared_ptr<const T>& a, const std::shared_ptr<const T>& b) {
                return take(asset::TAssetSerDeser<T>::encode(*a, asset::AssetEncodeLimits{1024 * 1024})) ==
                       take(asset::TAssetSerDeser<T>::encode(*b, asset::AssetEncodeLimits{1024 * 1024}));
            };
            assert(equal_asset(after.configuration().scene, before.configuration().scene));
            assert(equal_asset(after.configuration().world, before.configuration().world));
            assert(equal_asset(after.configuration().simulation, before.configuration().simulation));
            const auto history_after = take(detail::SceneSessionAccess::data(*f.session).history->view()).snapshot;
            assert(history_after.cursor == history_before.cursor);
            assert(history_after.entry_count == history_before.entry_count);
            assert(f.session->describe().current == baseline.current);
            assert(f.session->describe().observed == baseline.observed);
            assert(f.session->describe().dirty == baseline.dirty);
            return;
        }
        assert(result && result->effect == editing::EEditEffect::CHANGE);
        const auto value = take(f.session->read()).component(f.ref(id), schema);
        const auto expected = take(encodeSceneValue(replacement, f.metadata, ecs::WorldEntityMap{}, 4096));
        if (scenario == 2)
            assert(!value && value.error().code == ESceneEditError::INVALID_COMPONENT);
        else
        {
            std::printf("complete replacement payload preserved=%d\n", bool(value && *value == expected));
            std::fflush(stdout);
            assert(value && *value == expected);
        }
        const auto after = take(f.session->capture());
        assert(f.session->undo());
        assert(f.session->describe().current == baseline.current);
        assert(std::ranges::equal(take(f.session->capture()).objects(), before.objects()));
        assert(f.session->redo());
        assert(std::ranges::equal(take(f.session->capture()).objects(), after.objects()));
    }

    void identity()
    {
        Fixture a, b{{}, "different-root"};
        a.create("same");
        b.create("same");
        const auto other_package = package(a.metadata, "different-root");
        assert(other_package.scene->id() != a.input.scene->id());
        assert(!PreparedSceneReload::prepare(*a.session, take(SceneSource::create(other_package, a.metadata))));
        auto source = take(SceneSource::create(a.input, a.metadata));
        auto reload = take(PreparedSceneReload::prepare(*a.session, std::move(source)));
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
        auto reload = take(PreparedSceneReload::prepare(*f.session, std::move(source)));
        assert(reload.adopt(*f.session));
        assert(take(f.session->changesSince(recent)).status == ESceneChanges::RESET_REQUIRED);
        assert(!f.session->describe().dirty);
        const auto baseline = f.session->describe();
        auto prepared = take(PreparedSceneReload::prepare(*f.session, take(SceneSource::create(f.input, f.metadata))));
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
void codecReadRegression(int scenario);

int main(int argc, char** argv)
{
    const std::string_view test = argc > 1 ? argv[1] : "content";
    if (test == "snapshot-cost")
        snapshotCost();
    else if (test == "applicability")
        applicability();
    else if (test == "content")
        content();
    else if (test == "atomic")
        atomic();
    else if (test == "identity")
        identity();
    else if (test == "changes")
        changes();
    else if (test == "plugin")
        pluginSnapshot();
    else if (test.starts_with("mixed-"))
        mixedComponents(test.back() - '0');
    else if (test.starts_with("read-"))
        codecReadRegression(test.back() - '0');
    else
        return 2;
}
