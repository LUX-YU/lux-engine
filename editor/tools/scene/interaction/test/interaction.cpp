#include "../../../../authoring/scene/src/PreparedSceneReload.hpp"
#include <lux/engine/editor/scene/SceneInteraction.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/scene/CameraSchema.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
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

    void interaction()
    {
        Fixture f;
        f.create("a");
        auto key = take(f.store.key<SceneSession>(f.id));
        SceneInteractionGroup gesture(f.store.access<SceneSession>(), key, {1});
        SceneInteractionGroup independent(f.store.access<SceneSession>(), key, {2});
        assert(gesture.select({{f.ref(object("a"))}}));
        assert(independent.selection().objects.empty());
        const auto start = f.session->describe();
        const auto snapshot = take(f.session->capture());
        const auto original =
            take(take(f.session->read()).component(f.ref(object("a")), ecs::componentSchemaId("lux.ecs.Transform3D")));
        auto& data = lux::editor::scene::detail::SceneSessionAccess::data(*f.session);
        const auto history = take(data.history->view()).snapshot;
        const auto checkpoint = data.state.checkpoint().persisted();
        const auto unchanged = [&] {
            auto info = f.session->describe();
            assert(info.current == start.current && info.dirty == start.dirty && info.observed == start.observed);
            assert(info.binding == start.binding && checkpoint == data.state.checkpoint().persisted());
            auto now = take(f.session->capture());
            assert(std::ranges::equal(snapshot.objects(), now.objects()));
            assert(take(data.history->view()).snapshot.entry_count == history.entry_count);
        };
        assert(gesture.begin("field"));
        std::vector<VSceneEdit> preview;
        preview.push_back(f.translation(object("a"), 20));
        assert(gesture.preview(preview));
        unchanged();
        assert(gesture.cancel());
        unchanged();
        assert(gesture.begin("field"));
        preview.push_back(f.translation(object("a"), 20));
        assert(gesture.preview(preview));
        assert(gesture.commit());
        assert(take(data.history->view()).snapshot.entry_count == history.entry_count + 1);
        assert(f.session->undo());
        assert(take(take(f.session->read()).component(f.ref(object("a")), original.schema)) == original);
        assert(f.session->redo());
        assert(gesture.begin("stale"));
        preview.push_back(f.translation(object("a"), 30));
        assert(gesture.preview(preview));
        auto concurrent = f.batch();
        concurrent.edits.push_back(SceneEraseObject{f.ref(object("a"))});
        assert(f.session->apply(std::move(concurrent)));
        auto changed = f.session->describe();
        assert(!gesture.commit());
        assert(f.session->describe().current == changed.current);
        assert(gesture.synchronize() && !gesture.overlay() && gesture.selection().objects.empty());
        f.create("reload-target");
        const auto old_ref = f.ref(object("reload-target"));
        assert(gesture.select({{old_ref}}));
        auto reloaded = take(lux::editor::scene::detail::PreparedSceneReload::prepare(
            *f.session,
            take(SceneSource::create(f.input, f.metadata))
        ));
        assert(reloaded.adopt(*f.session));
        assert(gesture.synchronize() && gesture.selection().objects.empty());
        f.create("reload-target");
        assert(!gesture.select({{old_ref}}));
        assert(gesture.select({{f.ref(object("reload-target"))}}));
    }
}
int main()
{
    interaction();
    std::puts("PASS P08 scene preview/cancel, one field commit, conflict and selection invalidation");
}
