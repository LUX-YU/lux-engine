#include "../../model/src/PreparedMaterialReload.hpp"
#include <lux/engine/editor/material/MaterialInteraction.hpp>
#include <lux/engine/editor/material/MaterialSessionAccess.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
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
    void interaction()
    {
        Fixture f;
        auto key = take(f.store.key<MaterialSession>(f.id));
        MaterialInteraction gesture(f.store.access<MaterialSession>(), key);
        MaterialInteraction independent(f.store.access<MaterialSession>(), key);
        const Saved start(f);
        const auto node = f.first();
        assert(gesture.select({node}));
        assert(independent.selection().empty());
        assert(gesture.begin("drag"));
        std::vector<VMaterialEdit> preview;
        preview.push_back(MaterialPlaceNode{node, {50, 60}});
        assert(gesture.preview(preview) && preview.empty());
        assert(gesture.overlay()->edits.size() == 1);
        start.unchanged(f);
        assert(gesture.cancel());
        start.unchanged(f);
        assert(gesture.begin("drag"));
        preview.push_back(MaterialPlaceNode{node, {50, 60}});
        assert(gesture.preview(preview));
        const auto committed = take(gesture.commit());
        assert(committed.content != start.info.current);
        const auto history = f.history();
        assert(history.entry_count == start.history.entry_count + 1);
        assert(!gesture.overlay());
        const auto encoded = f.encoded();
        assert(encoded != start.bytes);
        assert(f.session->undo());
        assert(f.encoded() == start.bytes);
        assert(f.session->redo());
        assert(f.encoded() == encoded);
        assert(gesture.begin("stale"));
        preview.push_back(MaterialRename{"preview"});
        assert(gesture.preview(preview));
        auto concurrent = f.batch();
        concurrent.edits.push_back(MaterialRename{"other view"});
        assert(f.session->apply(std::move(concurrent)));
        const Saved after(f);
        assert(!gesture.commit());
        after.unchanged(f);
        assert(gesture.synchronize() && !gesture.overlay());
        after.unchanged(f);
        auto read = take(f.session->read());
        assert(read.withRead([&](const auto&) -> MaterialEditResult<void> {
            const auto denied = gesture.begin("nested");
            assert(!denied && denied.error().session == sessions::ESessionError::BUSY);
            return {};
        }));
        const auto current = f.session->describe().current;
        auto close = take(f.store.prepareClose(current));
        assert(f.store.close(close));
        assert(gesture.synchronize() && gesture.selection().empty());
        assert(!gesture.begin("closed"));
    }
}
int main()
{
    interaction();
    std::puts("PASS P08 material preview/cancel, one commit, persistent layout, conflict, gate, closed identity");
}
