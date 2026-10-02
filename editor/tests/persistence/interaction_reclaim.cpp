#include <lux/engine/editor/scene/SceneInteraction.hpp>
#include <lux/engine/editor/material/MaterialInteraction.hpp>
#include <lux/engine/editor/flowforge/FlowInteraction.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <cassert>
#include <cstdio>
#include <functional>
#include <thread>

#ifdef LUX_NATIVE_INTERACTION_RECLAIM
#include "../../authoring/flow/src/FlowSessionData.hpp"
#include "../../authoring/material/src/MaterialSessionData.hpp"
#include "../../authoring/scene/src/SceneSessionData.hpp"
#include <lux/engine/editor/scene/PreparedSceneReload.hpp>
#include <lux/engine/editor/material/PreparedMaterialReload.hpp>
#include <lux/engine/editor/flowforge/PreparedFlowReload.hpp>
#endif

namespace
{
    using namespace lux;
    using namespace lux::editor;
    namespace es = lux::editor::scene;
    namespace em = lux::editor::material;
    namespace ef = lux::editor::flowforge;
    using contracts::CodeLease;
    using sessions::ESessionError;

    template <class R> auto take(R result)
    {
        if (!result)
        {
            std::fputs("unexpected fixture failure\n", stderr);
            std::abort();
        }
        return std::move(*result);
    }
    const asset::AssetId root{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    const world::WorldObjectId object{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abd")};
    struct OnRelease final
    {
        std::function<void()> callback;
        ~OnRelease() noexcept
        {
            if (callback)
                callback();
        }
    };
    void close(sessions::SessionStore& store, sessions::SessionId id)
    {
        auto permit = take(store.prepareClose(take(store.describe(id)).current));
        assert(store.close(permit));
    }
    template <class R> bool busy(const R& result)
    {
        if (result)
            return false;
        const auto& error = result.error();
        if constexpr (std::same_as<std::decay_t<decltype(error)>, es::SceneEditError>)
        {
            // Preserve Scene's existing domain admission error as well as Store's typed access error.
            return error.code == es::ESceneEditError::BUSY ||
                   (error.code == es::ESceneEditError::SESSION && error.session == ESessionError::BUSY);
        }
        else
            return error.code == decltype(error.code)::SESSION && error.session == ESessionError::BUSY;
    }
    bool sameInfo(const sessions::SessionInfo& a, const sessions::SessionInfo& b)
    {
        return a.id == b.id && a.kind == b.kind && a.binding == b.binding && a.current == b.current &&
               a.observed == b.observed && a.dirty == b.dirty && a.admission == b.admission;
    }

    struct Material final
    {
        using Session = em::MaterialSession;
        using Interaction = em::MaterialInteraction;
        using Edit = em::VMaterialEdit;
        using Batch = em::MaterialEditBatch;
        static constexpr const char* name = "Material";
        static auto create(sessions::SessionStore& store, CodeLease code = CodeLease::builtin())
        {
            auto reservation = take(store.reserve<Session>({"lux.editor.material"}, std::move(code)));
            lux::material::MaterialSource source{root, "material", {}};
            (void)source.graph.addNode(std::make_unique<lux::material::ConstantNode>());
            auto owner =
                take(Session::create(reservation.id(), sessions::BoundSource{root, "a.material"}, std::move(source)));
            auto* pointer = owner.get();
            assert(store.prepare(reservation, owner));
            return std::pair{take(store.publish(reservation)), pointer};
        }
        static auto node(Session& s)
        {
            return take(s.capture()).source().graph.topology().nodes().front().id;
        }
        static void select(Interaction& g, Session& s)
        {
            assert(g.select({node(s)}));
        }
        static auto selection(const Interaction& g)
        {
            return std::vector(g.selection().begin(), g.selection().end());
        }
        static auto encode(Session& s)
        {
            return take(take(s.read()).encode());
        }
        static Edit payload(Session&, std::shared_ptr<OnRelease> owner)
        {
            return em::MaterialInsertNode{
                CodeLease::plugin(std::move(owner)),
                std::make_unique<lux::material::ConstantNode>(),
                {50, 60}
            };
        }
        static auto underRead(Session& s, auto&& fn)
        {
            return take(s.read()).withRead([&](const lux::material::MaterialSource&) -> em::MaterialEditResult<void> {
                fn();
                return {};
            });
        }
        static void erase(Session& s)
        {
            Batch batch{s.describe().current, "erase", {}};
            batch.edits.push_back(em::MaterialEraseNode{node(s)});
            assert(s.apply(std::move(batch)));
        }
#ifdef LUX_NATIVE_INTERACTION_RECLAIM
        static auto& data(Session& s)
        {
            return em::detail::MaterialSessionAccess::data(s);
        }
        static void reload(Session& s)
        {
            lux::material::MaterialSource source{root, "reloaded material", {}};
            (void)source.graph.addNode(std::make_unique<lux::material::ConstantNode>());
            auto candidate = take(em::PreparedMaterialReload::prepare(s, std::move(source)));
            assert(candidate.adopt(s));
        }
#endif
    };
    struct Flow final
    {
        using Session = ef::FlowSession;
        using Interaction = ef::FlowInteraction;
        using Edit = ef::VFlowEdit;
        using Batch = ef::FlowEditBatch;
        static constexpr const char* name = "Flow";
        static auto create(sessions::SessionStore& store, CodeLease code = CodeLease::builtin())
        {
            auto reservation = take(store.reserve<Session>({"lux.editor.flowforge"}, std::move(code)));
            ef::FlowAuthoringSource source{root, "flow", {}};
            (void)source.graph.addNodes(std::make_unique<lux::flowforge::StartNode>());
            auto owner =
                take(Session::create(reservation.id(), sessions::BoundSource{root, "a.flow"}, std::move(source)));
            auto* pointer = owner.get();
            assert(store.prepare(reservation, owner));
            return std::pair{take(store.publish(reservation)), pointer};
        }
        static auto node(Session& s)
        {
            return take(s.capture()).source().nodes.front().id;
        }
        static void select(Interaction& g, Session& s)
        {
            assert(g.select({node(s)}));
        }
        static auto selection(const Interaction& g)
        {
            return std::vector(g.selection().begin(), g.selection().end());
        }
        static auto encode(Session& s)
        {
            return take(take(s.read()).encode());
        }
        static Edit payload(Session&, std::shared_ptr<OnRelease> owner)
        {
            return ef::FlowInsertNode{
                CodeLease::plugin(std::move(owner)),
                std::make_unique<lux::flowforge::BranchNode>(),
                {50, 60}
            };
        }
        static auto underRead(Session& s, auto&& fn)
        {
            return take(s.read()).withRead([&]() -> ef::FlowEditResult<void> {
                fn();
                return {};
            });
        }
        static void erase(Session& s)
        {
            Batch batch{s.describe().current, "erase", {}};
            batch.edits.push_back(ef::FlowRemoveNodes{{node(s)}, {}});
            assert(s.apply(std::move(batch)));
        }
#ifdef LUX_NATIVE_INTERACTION_RECLAIM
        static auto& data(Session& s)
        {
            return ef::detail::FlowSessionAccess::data(s);
        }
        static void reload(Session& s)
        {
            ef::FlowAuthoringSource source{root, "reloaded flow", {}};
            (void)source.graph.addNodes(std::make_unique<lux::flowforge::StartNode>());
            auto candidate = take(ef::PreparedFlowReload::prepare(s, std::move(source)));
            assert(candidate.adopt(s));
        }
#endif
    };
    struct Scene final
    {
        using Session = es::SceneSession;
        using Interaction = es::SceneInteractionGroup;
        using Edit = es::VSceneEdit;
        using Batch = es::SceneEditBatch;
        static constexpr const char* name = "Scene";
        static auto create(sessions::SessionStore& store, CodeLease code = CodeLease::builtin())
        {
            namespace ecs = simulation::ecs;
            auto reservation = take(store.reserve<Session>({"lux.editor.scene"}, std::move(code)));
            std::vector<ecs::ComponentSchema> values;
            for (auto schema : ecs::transformComponentSchemas())
                if (schema.snapshot == ecs::EComponentSnapshotPolicy::COPY)
                    values.push_back(std::move(schema));
            auto schemas = take(ecs::ComponentSchemaSet::build(std::move(values)));
            std::vector<world::WorldDataSchemaId> ids;
            for (const auto& schema : schemas.all())
                ids.push_back(world::worldDataSchemaId(schema.id.name));
            auto rules = take(std::move(simulation::SimulationDescriptionBuilder{}).build());
            auto description = take(std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved());
            auto package = take(lux::scene::createScenePackage(
                root,
                "scene",
                ids,
                std::make_shared<const simulation::SimulationDescription>(std::move(rules)),
                description
            ));
            auto source = take(es::SceneSource::create(package, schemas));
            auto owner =
                take(Session::create(reservation.id(), sessions::BoundSource{root, "a.scene"}, std::move(source)));
            ecs::WorldEntityMap identities;
            Batch batch{owner->describe().current, "create", {}};
            batch.edits.push_back(es::SceneCreateObject{
                {object, {0}, {take(es::encodeSceneValue(ecs::Transform3D{}, schemas, identities, 4096))}}
            });
            assert(owner->apply(std::move(batch)));
            auto* pointer = owner.get();
            assert(store.prepare(reservation, owner));
            return std::pair{take(store.publish(reservation)), pointer};
        }
        static auto ref(Session& s)
        {
            return es::SceneObjectRef{s.describe().id, s.describe().current.state.history, object};
        }
        static void select(Interaction& g, Session& s)
        {
            assert(g.select({{ref(s)}}));
        }
        static auto selection(const Interaction& g)
        {
            return g.selection().objects;
        }
        static auto encode(Session& s)
        {
            auto package = take(es::buildSceneSnapshotPackage(take(s.capture())));
            return take(lux::scene::encodeScenePackage(package, 1024 * 1024));
        }
        static Edit payload(Session& s, std::shared_ptr<OnRelease> owner)
        {
            return es::SceneSetField::make<simulation::ecs::Transform3D>(
                {ref(s), simulation::ecs::componentSchemaId("lux.ecs.Transform3D"), "translation"},
                Eigen::Vector3d{50, 60, 70},
                std::move(owner)
            );
        }
        static auto underRead(Session& s, auto&& fn)
        {
            return take(s.read()).withRead([&](const es::SceneReadView&) -> es::SceneEditResult<void> {
                fn();
                return {};
            });
        }
        static void erase(Session& s)
        {
            Batch batch{s.describe().current, "erase", {}};
            batch.edits.push_back(es::SceneEraseObject{ref(s)});
            assert(s.apply(std::move(batch)));
        }
#ifdef LUX_NATIVE_INTERACTION_RECLAIM
        static auto& data(Session& s)
        {
            return es::detail::SceneSessionAccess::data(s);
        }
        static void reload(Session& s)
        {
            auto package = take(es::buildSceneSnapshotPackage(take(s.capture())));
            auto source =
                take(es::SceneSource::create(package, es::detail::SceneSourceAccess::data(data(s).source).schemas));
            auto candidate = take(es::PreparedSceneReload::prepare(s, std::move(source)));
            assert(candidate.adopt(s));
        }
#endif
    };

    template <class Domain> bool scenario(std::string_view mode)
    {
        using Session = typename Domain::Session;
        int released{};
        bool check_cleanup{};
        bool gate_held{};
        bool inactive_on_cleanup{};
        sessions::SessionStore store{2};
        auto [id, session] = Domain::create(store);
        const auto key = take(store.key<Session>(id));
        auto gesture = [&] {
            if constexpr (std::same_as<Domain, Scene>)
                return typename Domain::Interaction{store.access<Session>(), key, {1}};
            else
                return typename Domain::Interaction{store.access<Session>(), key};
        }();
        Domain::select(gesture, *session);
        const auto selection = Domain::selection(gesture);
        const auto original = session->describe();
        const auto encoded = Domain::encode(*session);
#ifdef LUX_NATIVE_INTERACTION_RECLAIM
        const auto history = take(Domain::data(*session).history->view()).snapshot;
        const auto checkpoint = Domain::data(*session).state.checkpoint().persisted();
#endif
        auto unchanged = [&] {
            assert(sameInfo(session->describe(), original));
            assert(Domain::encode(*session) == encoded);
#ifdef LUX_NATIVE_INTERACTION_RECLAIM
            const auto now = take(Domain::data(*session).history->view()).snapshot;
            assert(now.entry_count == history.entry_count && now.cursor == history.cursor);
            assert(now.current == history.current && now.history == history.history);
            assert(now.revision == history.revision && now.event_sequence == history.event_sequence);
            assert(now.charged_retained_bytes == history.charged_retained_bytes);
            assert(now.history_metadata_bytes == history.history_metadata_bytes && now.closed == history.closed);
            assert(Domain::data(*session).state.checkpoint().persisted() == checkpoint);
#endif
        };
        auto input = std::make_shared<OnRelease>();
        input->callback = [&] {
            ++released;
            if (check_cleanup)
            {
                gate_held = busy(session->read());
                inactive_on_cleanup = !gesture.overlay();
            }
        };
        const bool has_gesture = mode != "selection";
        if (has_gesture)
        {
            assert(gesture.begin("owned preview"));
            std::vector<typename Domain::Edit> edits;
            edits.push_back(Domain::payload(*session, std::move(input)));
            assert(gesture.preview(edits));
        }
        else
            input->callback = {};
        const auto* payload = gesture.overlay() ? gesture.overlay()->edits.data() : nullptr;
        auto retained = [&] {
            const auto* overlay = gesture.overlay();
            return bool(overlay) == has_gesture && Domain::selection(gesture) == selection && released == 0 &&
                   (!overlay || (overlay->expected == original.current && overlay->label == "owned preview" &&
                                 overlay->edits.size() == 1 && overlay->edits.data() == payload));
        };
        if (mode == "thread")
        {
            std::jthread caller([&] {
                const auto cancelled = gesture.cancel();
                assert(!cancelled && cancelled.error().session == ESessionError::WRONG_THREAD);
                const auto synced = gesture.synchronize();
                assert(!synced && synced.error().session == ESessionError::WRONG_THREAD);
                assert(retained());
            });
            caller.join();
            unchanged();
            assert(gesture.cancel());
            unchanged();
            std::printf("%s cancel/synchronize=WRONG_THREAD retained=1 author=unchanged\n", Domain::name);
            return true;
        }
#ifdef LUX_NATIVE_INTERACTION_RECLAIM
        if (mode == "reload")
        {
            Domain::reload(*session);
            assert(session->describe().current.state.history != original.current.state.history);
            const auto loaded = session->describe();
            const auto loaded_bytes = Domain::encode(*session);
            assert(!gesture.commit() && gesture.overlay());
            check_cleanup = true;
            assert(gesture.synchronize());
            assert(!gesture.overlay() && Domain::selection(gesture).empty());
            assert(released == 1 && gate_held && inactive_on_cleanup);
            assert(sameInfo(session->describe(), loaded) && Domain::encode(*session) == loaded_bytes);
            Domain::select(gesture, *session);
            assert(Domain::selection(gesture).size() == 1);
            std::printf("%s reload new-history=1 old-gesture/selection=cleared same-object-valid=1\n", Domain::name);
            return true;
        }
#endif
        if (mode == "gate")
        {
            assert(Domain::underRead(*session, [&] {
                assert(busy(gesture.cancel()) && retained());
                assert(busy(gesture.synchronize()) && retained());
            }));
            unchanged();
            check_cleanup = true;
            assert(gesture.cancel());
            assert(gate_held && inactive_on_cleanup && released == 1);
            unchanged();
            assert(gesture.cancel());
            std::printf(
                "%s gate cancel/synchronize=BUSY preserved=1 cleanup=1 gate_held=1 inactive=1 author=unchanged\n",
                Domain::name
            );
            return true;
        }
        if (mode == "stale")
        {
            close(store, id);
            const auto absent = store.access<Session>().read(key);
            assert(!absent && absent.error() == ESessionError::STALE_SESSION);
            auto [next_id, next] = Domain::create(store);
            assert(next_id.domain == id.domain && next_id.slot == id.slot && next_id.generation != id.generation);
            const auto next_info = next->describe();
            const auto next_encoded = Domain::encode(*next);
            assert(gesture.synchronize());
            assert(!gesture.overlay() && Domain::selection(gesture).empty() && released == 1);
            assert(gesture.cancel() && gesture.synchronize());
            assert(sameInfo(next->describe(), next_info) && Domain::encode(*next) == next_encoded);
            std::printf("%s stale slot-reuse cleanup=1 payload=1 new-author=unchanged\n", Domain::name);
            return true;
        }
        bool called{}, lookup_busy{}, accepted{}, reported_busy{}, preserved{};
        auto hook = std::make_shared<OnRelease>();
        auto [other_id, other] = Domain::create(store, CodeLease::plugin(hook));
        hook->callback = [&] {
            called = true;
            auto lookup = store.access<Session>().read(key);
            lookup_busy = !lookup && lookup.error() == ESessionError::BUSY;
            auto result = mode == "cancel" ? gesture.cancel() : gesture.synchronize();
            accepted = bool(result);
            reported_busy = busy(result);
            preserved = retained();
        };
        hook.reset();
        close(store, other_id);
        unchanged();
        std::printf(
            "%s operation=%.*s callback=%d store_busy=%d live=%d result_ok=%d reported_busy=%d "
            "overlay=%d selection=%zu stamp_payload_retained=%d released=%d author=unchanged\n",
            Domain::name,
            int(mode.size()),
            mode.data(),
            called,
            lookup_busy,
            bool(store.describe(id)),
            accepted,
            reported_busy,
            bool(gesture.overlay()),
            Domain::selection(gesture).size(),
            preserved,
            released
        );
        std::fflush(stdout);
        if (!(called && lookup_busy && !accepted && reported_busy && preserved))
            return false;
        assert(gesture.synchronize() && retained());
        if (mode == "sync")
        {
            assert(gesture.commit());
            const auto committed = Domain::encode(*session);
            assert(committed != encoded && session->describe().current != original.current);
#ifdef LUX_NATIVE_INTERACTION_RECLAIM
            assert(take(Domain::data(*session).history->view()).snapshot.entry_count == history.entry_count + 1);
#endif
            assert(session->undo());
            assert(session->describe().current == original.current && Domain::encode(*session) == encoded);
            assert(session->redo() && Domain::encode(*session) == committed);
        }
        else if (mode == "cancel")
        {
            check_cleanup = true;
            assert(gesture.cancel());
            assert(released == 1 && gate_held && inactive_on_cleanup);
            assert(Domain::selection(gesture) == selection && gesture.cancel());
            unchanged();
        }
        else
        {
            assert(gesture.cancel());
            unchanged();
            Domain::erase(*session);
            assert(gesture.synchronize() && Domain::selection(gesture).empty());
        }
        std::printf(
            "%s retry=%.*s PASS original-history/checkpoint preserved, real domain follow-up verified\n",
            Domain::name,
            int(mode.size()),
            mode.data()
        );
        return true;
    }
}
int main(int argc, char** argv)
{
    assert(argc == 3);
    const std::string_view domain = argv[1], mode = argv[2];
    if (domain == "scene")
        return scenario<Scene>(mode) ? 0 : 1;
    if (domain == "material")
        return scenario<Material>(mode) ? 0 : 1;
    if (domain == "flow")
        return scenario<Flow>(mode) ? 0 : 1;
    return 2;
}
