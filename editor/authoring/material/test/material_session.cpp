#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include "../src/PreparedMaterialReload.hpp"
#include <cassert>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>

namespace
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::material;
    namespace mat = lux::material;
    template <class T> auto take(T result)
    {
        if (!result)
        {
            if constexpr (requires { result.error().code; })
                std::fprintf(stderr, "unexpected error %u\n", unsigned(result.error().code));
            std::abort();
        }
        return std::move(*result);
    }
    asset::AssetId rootId()
    {
        return asset::AssetId{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    }
    mat::MaterialSource source()
    {
        mat::MaterialSource result{rootId(), "test material", {}};
        const auto id = result.graph.addNode(std::make_unique<mat::ConstantNode>());
        assert(id.valid());
        assert(result.graph.layout().set(id, {1, 2}));
        return result;
    }
    struct Fixture final
    {
        sessions::SessionStore store{8};
        MaterialSession* session{};
        sessions::SessionId id;
        Fixture(
            mat::MaterialSource input = source(),
            contracts::CodeLease code = contracts::CodeLease::builtin(),
            MaterialSessionLimits limits = {},
            bool bound = true
        )
        {
            auto reservation =
                take(store.reserve<MaterialSession>({"lux.editor.material"}, contracts::CodeLease::builtin()));
            id = reservation.id();
            auto candidate = take(MaterialSession::create(
                id,
                bound ? sessions::SourceBinding{sessions::BoundSource{input.id, "test.luxmaterial"}} : std::nullopt,
                std::move(input),
                code,
                limits
            ));
            session = candidate.get();
            assert(store.prepare(reservation, candidate));
            assert(store.publish(reservation));
        }
        MaterialEditBatch batch() const
        {
            return {session->describe().current, "material edit", {}};
        }
        std::string encoded() const
        {
            return take(take(session->read()).encode());
        }
        mat::NodeId first() const
        {
            return take(take(session->read()).withRead([](const auto& value) -> MaterialEditResult<mat::NodeId> {
                return value.graph.topology().nodes().front().id;
            }));
        }
        editing::HistorySnapshot history() const
        {
            return take(lux::editor::material::detail::MaterialSessionAccess::data(*session).history->view()).snapshot;
        }
    };
    void sameHistory(const editing::HistorySnapshot& a, const editing::HistorySnapshot& b)
    {
        assert(a.history == b.history && a.current == b.current && a.revision == b.revision);
        assert(a.event_sequence == b.event_sequence && a.entry_count == b.entry_count && a.cursor == b.cursor);
        assert(a.charged_retained_bytes == b.charged_retained_bytes && a.closed == b.closed);
    }
    struct Saved final
    {
        std::string bytes;
        sessions::SessionInfo info;
        editing::HistorySnapshot history;
        sessions::BindingRevision binding_revision;
        std::optional<sessions::PersistedState> persisted;
        explicit Saved(const Fixture& f) : bytes(f.encoded()), info(f.session->describe()), history(f.history())
        {
            const auto& state = lux::editor::material::detail::MaterialSessionAccess::data(*f.session).state;
            binding_revision = state.bindingRevision();
            persisted = state.checkpoint().persisted();
        }
        void unchanged(const Fixture& f) const
        {
            assert(bytes == f.encoded());
            const auto now = f.session->describe();
            assert(info.current == now.current && info.observed == now.observed && info.dirty == now.dirty);
            assert(info.id == now.id && info.binding == now.binding && info.admission == now.admission);
            const auto& state = lux::editor::material::detail::MaterialSessionAccess::data(*f.session).state;
            assert(binding_revision == state.bindingRevision() && persisted == state.checkpoint().persisted());
            sameHistory(history, f.history());
        }
        void report(const Fixture& f) const
        {
            const auto now = f.session->describe();
            const auto h = f.history();
            const bool same_history = history.history == h.history && history.current == h.current &&
                                      history.revision == h.revision && history.event_sequence == h.event_sequence &&
                                      history.entry_count == h.entry_count && history.cursor == h.cursor &&
                                      history.charged_retained_bytes == h.charged_retained_bytes &&
                                      history.closed == h.closed;
            std::printf(
                "source_unchanged=%d history_unchanged=%d observed_unchanged=%d dirty_unchanged=%d "
                "binding_unchanged=%d\n",
                bytes == f.encoded(),
                same_history,
                info.observed == now.observed,
                info.dirty == now.dirty,
                info.binding == now.binding
            );
            std::fflush(stdout);
            unchanged(f);
        }
    };
    void content()
    {
        Fixture f;
        const Saved original(f);
        auto batch = f.batch();
        batch.edits.push_back(MaterialRename{"author graph"});
        batch.edits.push_back(MaterialSetConstant{f.first(), {1, 2, 3, 4}});
        batch.edits.push_back(MaterialSetParameterSlots{{{"gain", mat::EValueType::FLOAT, {2, 0, 0, 0}}}});
        batch.edits.push_back(MaterialSetTextureSlots{{{"albedo", {}}}});
        batch.edits.push_back(MaterialSetShading{rdesc::ELightingTechnique::GRAPH});
        batch.edits.push_back(MaterialSetRenderState{{rdesc::EAlphaMode::BLEND, .7f, true}});
        batch.edits.push_back(
            MaterialInsertNode{{contracts::CodeLease::builtin()}, std::make_unique<mat::MathNode>(), {5, 6}}
        );
        const auto added = take(f.session->apply(std::move(batch))).inserted.front();
        assert(f.history().entry_count == 1 && f.session->describe().dirty);
        auto snapshot = take(f.session->capture());
        const auto from = snapshot.source().graph.node(f.first())->outputs().front().id;
        const auto to = snapshot.source().graph.node(added)->inputs().front().id;
        batch = f.batch();
        batch.edits.push_back(MaterialConnect{from, to});
        assert(f.session->apply(std::move(batch)));
        const auto connected = f.encoded();
        batch = f.batch();
        batch.edits.push_back(MaterialConnect{from, to});
        Saved no_change(f);
        assert(take(f.session->apply(std::move(batch))).effect == editing::EEditEffect::NO_CHANGE);
        no_change.unchanged(f);
        batch = f.batch();
        batch.edits.push_back(MaterialEraseNode{f.first()});
        assert(f.session->apply(std::move(batch)));
        assert(take(f.session->capture()).source().graph.topology().links().empty());
        assert(f.session->undo());
        assert(f.encoded() == connected);
        assert(f.session->undo());
        assert(f.session->undo());
        assert(f.encoded() == original.bytes && !f.session->describe().dirty);
        assert(f.session->redo());
        assert(f.session->redo());
        assert(f.encoded() == connected);
        const auto key = take(f.store.key<MaterialSession>(f.id));
        assert(f.store.access<MaterialSession>().read(key));
        Fixture slots;
        auto slot_edit = slots.batch();
        slot_edit.edits.push_back(MaterialSetParameterSlots{{{"parameter", mat::EValueType::FLOAT, {}}}});
        slot_edit.edits.push_back(MaterialInsertNode{
            contracts::CodeLease::builtin(),
            std::make_unique<mat::ParamNode>(mat::EValueType::FLOAT),
            {}
        });
        const auto parameter_id = take(slots.session->apply(std::move(slot_edit))).inserted.front();
        const auto slot_before = slots.encoded();
        auto parameter = take(slots.session->capture()).source().graph.node(parameter_id)->clone();
        parameter->as<mat::ParamNode>()->setType(mat::EValueType::VEC4);
        slot_edit = slots.batch();
        slot_edit.edits.push_back(MaterialSetParameterSlots{{{"parameter", mat::EValueType::VEC4, {}}}});
        slot_edit.edits.push_back(MaterialReplaceNode{contracts::CodeLease::builtin(), std::move(parameter)});
        assert(slots.session->apply(std::move(slot_edit)));
        const auto slot_after = slots.encoded();
        assert(slots.session->undo() && slots.encoded() == slot_before);
        assert(slots.session->redo() && slots.encoded() == slot_after);
        const Saved slot_saved(slots);
        slot_edit = slots.batch();
        slot_edit.edits.push_back(MaterialSetParameterSlots{});
        assert(!slots.session->apply(std::move(slot_edit)));
        slot_saved.unchanged(slots);
        slot_edit = slots.batch();
        slot_edit.edits.push_back(MaterialEraseNode{parameter_id});
        slot_edit.edits.push_back(MaterialSetParameterSlots{});
        assert(slots.session->apply(std::move(slot_edit)));
        assert(slots.session->undo() && slots.encoded() == slot_after);
        std::puts("X03-01 CPU graph, slots, links, root identity, one History and exact undo/redo PASS");
    }
    std::function<void()> clone_hook, destroy_hook;
    std::size_t alive{}, clones{}, destroyed{};
    bool released_early{};
    bool invalid_clone{};
    class PluginConstant final : public mat::ConstantNode
    {
    public:
        explicit PluginConstant(std::weak_ptr<const void> code) : code_(std::move(code))
        {
            ++alive;
        }
        PluginConstant(const PluginConstant& other) : mat::ConstantNode(other), code_(other.code_)
        {
            ++alive;
        }
        ~PluginConstant() override
        {
            released_early |= code_.expired();
            if (auto hook = std::exchange(destroy_hook, {}))
                hook();
            --alive;
            ++destroyed;
        }
        std::unique_ptr<mat::Node> clone() const override
        {
            ++clones;
            if (auto hook = std::exchange(clone_hook, {}))
                hook();
            auto result = std::make_unique<PluginConstant>(*this);
            if (invalid_clone)
                result->value[0] = std::numeric_limits<float>::quiet_NaN();
            return result;
        }

    private:
        std::weak_ptr<const void> code_;
    };
    void failure()
    {
        auto code = std::make_shared<int>(1);
        Fixture f;
        auto batch = f.batch();
        batch.edits.push_back(
            MaterialInsertNode{contracts::CodeLease::plugin(code), std::make_unique<PluginConstant>(code), {}}
        );
        batch.edits.push_back(MaterialDisconnect{{999}, {998}});
        const Saved saved(f);
        assert(!f.session->apply(std::move(batch)));
        saved.unchanged(f);
        assert(alive == 0 && !released_early);
        auto replacement = std::make_unique<PluginConstant>(code);
        replacement->setId(f.first()); // Pins intentionally do not preserve the existing identity.
        batch = f.batch();
        batch.edits.push_back(MaterialReplaceNode{contracts::CodeLease::plugin(code), std::move(replacement)});
        assert(!f.session->apply(std::move(batch)));
        saved.unchanged(f);
        assert(alive == 0 && !released_early);
        MaterialSessionLimits limits;
        limits.history.max_staging_bytes = 1;
        Fixture tiny(source(), contracts::CodeLease::builtin(), limits);
        Saved small(tiny);
        batch = tiny.batch();
        batch.edits.push_back(MaterialRename{"other"});
        assert(!tiny.session->apply(std::move(batch)));
        small.unchanged(tiny);
        limits.history.max_staging_bytes = 1024 * 1024;
        limits.history.max_retained_bytes = 1;
        Fixture retained(source(), contracts::CodeLease::builtin(), limits);
        Saved unchanged(retained);
        batch = retained.batch();
        batch.edits.push_back(MaterialRename{"other"});
        assert(!retained.session->apply(std::move(batch)));
        unchanged.unchanged(retained);
        std::printf(
            "X03-02 rejected candidates: alive=%zu destroyed=%zu, exact source/history unchanged PASS\n",
            alive,
            destroyed
        );
    }
    void layout()
    {
        Fixture f;
        const auto original = f.encoded();
        auto batch = f.batch();
        batch.edits.push_back(MaterialPlaceNode{f.first(), {42, -7}});
        assert(f.session->apply(std::move(batch)));
        auto snapshot = take(f.session->capture());
        assert(snapshot.source().graph.layout().find(f.first())->x == 42);
        const auto moved = take(mat::encodeMaterialSource(snapshot.source()));
        assert(take(mat::decodeMaterialSource(moved)).graph.layout().find(f.first())->y == -7);
        struct View
        {
            float pan{}, zoom{1};
        } view;
        view.pan = 100;
        view.zoom = 3;
        assert(f.encoded() == moved && snapshot.source().graph.layout().find(f.first())->x == 42);
        assert(f.session->undo());
        assert(f.encoded() == original);
        assert(f.session->redo());
        assert(f.encoded() == moved);
        std::puts("X03-03 authored layout round-trip/undo; local pan/zoom excluded PASS");
    }
    void lifetime()
    {
        bool released{};
        auto code = std::shared_ptr<int>(new int(1), [&](int* p) {
            assert(alive == 0);
            released = true;
            delete p;
        });
        MaterialSnapshot snapshot;
        {
            auto input = source();
            const auto id = input.graph.addNode(std::make_unique<PluginConstant>(code));
            Fixture f(std::move(input), contracts::CodeLease::plugin(code));
            code.reset();
            snapshot = take(f.session->capture());
            const auto count = clones;
            auto batch = f.batch();
            batch.edits.push_back(MaterialSetConstant{f.first(), {5, 6, 7, 8}});
            assert(f.session->apply(std::move(batch)));
            assert(clones == count); // local edit never clones unrelated node
            auto replacement = snapshot.source().graph.node(id)->clone();
            replacement->as<mat::ConstantNode>()->value[0] = 99;
            batch = f.batch();
            batch.edits.push_back(MaterialReplaceNode{contracts::CodeLease::builtin(), std::move(replacement)});
            assert(f.session->apply(std::move(batch)));
            assert(snapshot.source().graph.node(id)->as<mat::ConstantNode>()->value[0] == 0);
            auto permit = take(f.store.prepareClose(f.session->describe().current));
            assert(f.store.close(permit));
            assert(!released && alive > 0);
        }
        assert(!released);
        snapshot = MaterialSnapshot{};
        assert(released && alive == 0 && !released_early);
        std::puts("X03-04 virtual clone/destructor code lease and late immutable snapshot PASS");
    }
    void mixed(bool recreate)
    {
        Fixture f;
        const auto id = f.first();
        const Saved original(f);
        auto replacement = take(f.session->capture()).source().graph.node(id)->clone();
        auto* constant = replacement->as<mat::ConstantNode>();
        constant->value[0] = 40;
        constant->value[1] = 50;
        constant->setName("replacement");
        constant->setType(mat::EValueType::FLOAT);
        auto batch = f.batch();
        batch.edits.push_back(MaterialSetConstant{id, {1, 2, 3, 4}});
        if (recreate)
        {
            batch.edits.push_back(MaterialEraseNode{id});
            batch.edits.push_back(MaterialInsertNode{contracts::CodeLease::builtin(), std::move(replacement), {8, 9}});
        }
        else
            batch.edits.push_back(MaterialReplaceNode{contracts::CodeLease::builtin(), std::move(replacement)});
        batch.edits.push_back(MaterialSetConstant{id, {7, 8, 9, 10}});
        assert(f.session->apply(std::move(batch)));
        auto frozen = take(f.session->capture());
        const auto* node = frozen.source().graph.node(id)->as<mat::ConstantNode>();
        assert(node->value[0] == 7 && node->name() == "replacement" && node->value_type == mat::EValueType::FLOAT);
        const auto after = f.encoded();
        assert(f.history().entry_count == 1);
        assert(f.session->undo());
        assert(f.encoded() == original.bytes && !f.session->describe().dirty);
        assert(f.session->redo());
        assert(f.encoded() == after);
        Saved previous(f);
        batch = f.batch();
        batch.edits.push_back(MaterialEraseNode{id});
        batch.edits.push_back(MaterialSetConstant{id, {0, 0, 0, 0}});
        assert(!f.session->apply(std::move(batch)));
        previous.unchanged(f);
        batch = f.batch();
        batch.edits.push_back(MaterialSetConstant{id, {100, 0, 0, 0}});
        batch.edits.push_back(MaterialSetConstant{id, {7, 8, 9, 10}});
        assert(take(f.session->apply(std::move(batch))).effect == editing::EEditEffect::NO_CHANGE);
        previous.unchanged(f);
        std::printf(
            "mixed %s exact graph/state/undo/redo/invalid order/no-change PASS\n",
            recreate ? "recreate" : "replace"
        );
    }
    void reading()
    {
        auto code = std::make_shared<int>(1);
        auto input = source();
        input.graph.addNode(std::make_unique<PluginConstant>(code));
        Fixture f(std::move(input), contracts::CodeLease::plugin(code));
        const Saved saved(f);
        auto candidate = take(lux::editor::material::detail::PreparedMaterialReload::prepare(*f.session, source()));
        const auto check = [&] {
            auto edit = f.batch();
            edit.edits.push_back(MaterialRename{"nested"});
            assert(!f.session->apply(std::move(edit)));
            assert(!f.session->undo() && !f.session->redo());
            assert(!f.store.prepareClose(f.session->describe().current));
            auto& owner = lux::editor::material::detail::MaterialSessionAccess::data(*f.session);
            assert(!owner.state.prepareBindingChange(owner.content(), owner.content()));
            assert(!candidate.adopt(*f.session));
            assert(!f.session->read());
        };
        clone_hook = check;
        assert(f.session->capture());
        saved.unchanged(f);
        clone_hook = [&] {
            check();
            throw std::runtime_error("foreign clone failure");
        };
        assert(!f.session->capture());
        saved.unchanged(f);
        invalid_clone = true;
        clone_hook = [&] { destroy_hook = check; };
        assert(!f.session->capture());
        invalid_clone = false;
        saved.unchanged(f);
        clone_hook = [&] { destroy_hook = check; };
        // withRead also covers temporary destructors, error returns and exception unwinding.
        auto read = take(f.session->read());
        assert(read.withRead([&](const auto& source) -> MaterialEditResult<void> {
            auto temporary = source.graph.clone();
            return {};
        }));
        saved.unchanged(f);
        bool caught{};
        try
        {
            static_cast<void>(read.withRead([&](const auto&) -> MaterialEditResult<void> {
                check();
                throw std::runtime_error("read callback");
            }));
        }
        catch (const std::runtime_error&)
        {
            caught = true;
        }
        assert(caught);
        saved.unchanged(f);
        auto batch = f.batch();
        batch.edits.push_back(MaterialRename{"after read"});
        assert(f.session->apply(std::move(batch)));
        assert(!candidate.adopt(*f.session));
        candidate = take(lux::editor::material::detail::PreparedMaterialReload::prepare(*f.session, source()));
        const auto old = f.session->describe().current;
        assert(candidate.adopt(*f.session));
        assert(
            f.session->describe().id == old.session && f.session->describe().current.state.history != old.state.history
        );
        assert(!f.session->describe().dirty);
        std::puts("read gate: callbacks, destructors, failure unwind, reentry, stale/atomic reload PASS");
    }
    void editAfterReload(Fixture& f)
    {
        auto edit = f.batch();
        edit.edits.push_back(MaterialRename{"after rejected reload"});
        assert(f.session->apply(std::move(edit)));
        assert(f.session->describe().admission == sessions::EEditAdmission::AVAILABLE);
        std::puts("normal_public_edit_after_gate_release=1");
    }
    void reloadUnbound(bool last_lease)
    {
        Fixture f(source(), contracts::CodeLease::builtin(), {}, false);
        const Saved saved(f);
        bool released{}, released_with_nodes{}, nested_edit{}, callback_ran{};
        sessions::EEditAdmission callback_admission{};
        auto code = std::shared_ptr<int>(new int(1), [&](int* p) {
            released = true;
            released_with_nodes = alive != 0;
            delete p;
        });
        auto input = source();
        input.graph.addNode(std::make_unique<PluginConstant>(code));
        input.graph.addNode(std::make_unique<PluginConstant>(code));
        auto lease = contracts::CodeLease::plugin(code);
        if (last_lease)
            code.reset();
        destroy_hook = [&] {
            callback_ran = true;
            callback_admission = f.session->describe().admission;
            auto edit = f.batch();
            edit.edits.push_back(MaterialRename{"destructor must not edit"});
            const auto result = f.session->apply(std::move(edit));
            nested_edit = bool(result);
            if (!result)
                assert(result.error().session == sessions::ESessionError::BUSY);
        };
        const auto result = lux::editor::material::detail::PreparedMaterialReload::prepare(
            *f.session,
            std::move(input),
            std::move(lease)
        );
        std::printf(
            "unbound last_lease=%d rejected=%d nested_edit=%d callback_admission=%u destroyed=%zu "
            "alive=%zu released_early=%d released_with_nodes=%d released=%d\n",
            last_lease,
            !result,
            nested_edit,
            unsigned(callback_admission),
            destroyed,
            alive,
            released_early,
            released_with_nodes,
            released
        );
        saved.report(f);
        assert(!result && result.error().code == EMaterialEditError::INVALID_SOURCE);
        assert(callback_ran && !nested_edit && callback_admission == sessions::EEditAdmission::READING);
        assert(destroyed == 2 && alive == 0 && !released_early && !released_with_nodes);
        assert(released == last_lease);
        code.reset();
        assert(released);
        editAfterReload(f);
    }
    void reloadBusy(bool reading)
    {
        Fixture f;
        const Saved saved(f);
        auto& state = lux::editor::material::detail::MaterialSessionAccess::data(*f.session).state;
        const auto admission = reading ? sessions::EEditAdmission::READING : sessions::EEditAdmission::CLOSING;
        const auto attempt = [&] {
            bool released{}, released_with_nodes{}, callback_ran{}, nested_edit{};
            auto code = std::shared_ptr<int>(new int(1), [&](int* p) {
                released = true;
                released_with_nodes = alive != 0;
                delete p;
            });
            auto input = source();
            input.graph.addNode(std::make_unique<PluginConstant>(code));
            input.graph.addNode(std::make_unique<PluginConstant>(code));
            auto lease = contracts::CodeLease::plugin(code);
            code.reset();
            destroy_hook = [&] {
                callback_ran = true;
                assert(state.admission() == admission);
                auto edit = f.batch();
                edit.edits.push_back(MaterialRename{"busy destructor"});
                const auto result = f.session->apply(std::move(edit));
                nested_edit = bool(result);
                assert(!result && result.error().session == sessions::ESessionError::BUSY);
            };
            const auto result = lux::editor::material::detail::PreparedMaterialReload::prepare(
                *f.session,
                std::move(input),
                std::move(lease)
            );
            std::printf(
                "busy outer=%u rejected=%d gate_unchanged=%d nested_edit=%d destroyed=%zu "
                "alive=%zu released_early=%d released_with_nodes=%d released=%d\n",
                unsigned(admission),
                !result,
                state.admission() == admission,
                nested_edit,
                destroyed,
                alive,
                released_early,
                released_with_nodes,
                released
            );
            std::fflush(stdout);
            assert(!result && result.error().session == sessions::ESessionError::BUSY);
            assert(callback_ran && !nested_edit && state.admission() == admission);
            assert(destroyed == 2 && alive == 0 && !released_early && !released_with_nodes && released);
        };
        if (reading)
        {
            assert(take(f.session->read()).withRead([&](const auto&) -> MaterialEditResult<void> {
                attempt();
                return {};
            }));
        }
        else
        {
            auto permit = take(f.store.prepareClose(saved.info.current));
            attempt();
        }
        saved.report(f);
        editAfterReload(f);
    }
    void reloadOutcomes()
    {
        Fixture f;
        auto edit = f.batch();
        edit.edits.push_back(MaterialRename{"unsaved author content"});
        assert(f.session->apply(std::move(edit)) && f.session->describe().dirty);
        const Saved saved(f);
        const auto frozen = take(f.session->capture());
        for (const bool throws : {false, true})
        {
            bool released{}, released_with_nodes{}, callback_ran{};
            auto code = std::shared_ptr<int>(new int(1), [&](int* p) {
                released = true;
                released_with_nodes = alive != 0;
                delete p;
            });
            auto input = source();
            input.graph.addNode(std::make_unique<PluginConstant>(code));
            if (!throws)
                input.id = asset::AssetId{*uuids::uuid::from_string("87654321-1234-1234-1234-123456789abc")};
            auto lease = contracts::CodeLease::plugin(code);
            code.reset();
            const auto check = [&] {
                callback_ran = true;
                assert(f.session->describe().admission == sessions::EEditAdmission::READING);
                auto nested = f.batch();
                nested.edits.push_back(MaterialRename{"failed preparation callback"});
                const auto result = f.session->apply(std::move(nested));
                assert(!result && result.error().session == sessions::ESessionError::BUSY);
            };
            destroy_hook = check;
            if (throws)
                clone_hook = [&] {
                    check();
                    throw std::runtime_error("reload input clone failure");
                };
            const auto result = lux::editor::material::detail::PreparedMaterialReload::prepare(
                *f.session,
                std::move(input),
                std::move(lease)
            );
            std::printf(
                "bound %s rejected=%d callback_ran=%d alive=%zu released_early=%d released_with_nodes=%d released=%d\n",
                throws ? "clone-exception" : "wrong-asset",
                !result,
                callback_ran,
                alive,
                released_early,
                released_with_nodes,
                released
            );
            saved.report(f);
            assert(
                !result &&
                result.error().code == (throws ? EMaterialEditError::CALLBACK : EMaterialEditError::INVALID_SOURCE)
            );
            assert(callback_ran && alive == 0 && !released_early && !released_with_nodes && released);
        }
        bool released{}, released_with_nodes{}, clone_ran{}, destroyed_input{};
        auto code = std::shared_ptr<int>(new int(1), [&](int* p) {
            released = true;
            released_with_nodes = alive != 0;
            delete p;
        });
        auto input = source();
        input.name = "reloaded material";
        input.graph.addNode(std::make_unique<PluginConstant>(code));
        const auto expected = take(mat::encodeMaterialSource(input));
        auto lease = contracts::CodeLease::plugin(code);
        code.reset();
        const auto check = [&] {
            assert(f.session->describe().admission == sessions::EEditAdmission::READING);
            auto nested = f.batch();
            nested.edits.push_back(MaterialRename{"successful preparation callback"});
            const auto result = f.session->apply(std::move(nested));
            assert(!result && result.error().session == sessions::ESessionError::BUSY);
        };
        clone_hook = [&] {
            clone_ran = true;
            check();
        };
        destroy_hook = [&] {
            destroyed_input = true;
            check();
        };
        auto prepared = take(lux::editor::material::detail::PreparedMaterialReload::prepare(
            *f.session,
            std::move(input),
            std::move(lease)
        ));
        saved.report(f);
        assert(clone_ran && destroyed_input && alive == 1 && !released);
        assert(prepared.adopt(*f.session));
        const auto now = f.session->describe();
        const auto& state = lux::editor::material::detail::MaterialSessionAccess::data(*f.session).state;
        assert(f.encoded() == expected && now.id == saved.info.id && now.binding == saved.info.binding);
        assert(now.current.state.history != saved.info.current.state.history && !now.dirty);
        assert(
            now.observed.value == saved.info.observed.value + 1 && state.bindingRevision() == saved.binding_revision
        );
        assert(state.checkpoint().persisted()->state == now.current.state && f.history().entry_count == 0);
        assert(take(mat::encodeMaterialSource(frozen.source())) == saved.bytes);
        const Saved adopted(f);
        assert(!prepared.adopt(*f.session));
        adopted.report(f);
        auto permit = take(f.store.prepareClose(now.current));
        assert(f.store.close(permit));
        assert(alive == 0 && released && !released_early && !released_with_nodes);
        std::puts("bound identity/exception cleanup, atomic reload, stale rejection and last lease release PASS");
    }
}
int main(int argc, char** argv)
{
    const std::string_view test = argc > 1 ? argv[1] : "content";
    if (test == "content")
        content();
    else if (test == "failure")
        failure();
    else if (test == "layout")
        layout();
    else if (test == "lifetime")
        lifetime();
    else if (test == "mixed-replace")
        mixed(false);
    else if (test == "mixed-recreate")
        mixed(true);
    else if (test == "reading")
        reading();
    else if (test == "reload-unbound")
        reloadUnbound(false);
    else if (test == "reload-unbound-lease")
        reloadUnbound(true);
    else if (test == "reload-closing-lease")
        reloadBusy(false);
    else if (test == "reload-reading-lease")
        reloadBusy(true);
    else if (test == "reload-outcomes")
        reloadOutcomes();
    else
        return 2;
}
