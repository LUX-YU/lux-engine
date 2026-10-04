#include "ObjectQueue.hpp"
#include "../../../../authoring/flow/src/FlowSessionData.hpp"
#include <lux/engine/editor/flowforge/PreparedFlowReload.hpp>
#include <lux/engine/editor/flowforge/FlowInteraction.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::flowforge;
    namespace flow = lux::flowforge;
    namespace access = lux::editor::flowforge::detail;
    template <class T> auto take(T result)
    {
        if (!result)
        {
            if constexpr (requires { result.error().code; })
                std::fprintf(stderr, "unexpected error %u\n", unsigned(result.error().code));
            if constexpr (requires { result.error().history; })
                std::fprintf(
                    stderr,
                    "history %u domain %llu\n",
                    unsigned(result.error().history.code),
                    result.error().history.domain_code
                );
            std::abort();
        }
        return std::move(*result);
    }
    asset::AssetId rootId()
    {
        return asset::AssetId{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    }
    FlowAuthoringSource source()
    {
        FlowAuthoringSource result{rootId(), "flow source", {}};
        auto event = result.graph.addNodes(std::make_unique<flow::OnEventNode>("tick"));
        assert(result.graph.addExport({flow::FlowForgeExportNodeId{1}, result.graph.getNode(event).node->id(), 1234}));
        (void)result.graph.addNodes(std::make_unique<flow::BranchNode>());
        (void)result.graph.addNodes(std::make_unique<flow::FuncDefNode>(
            "function",
            std::vector<flow::FuncArgInfo>{{&meta::ref_type_of_v<bool>, "condition"}},
            std::vector<flow::FuncArgInfo>{{&meta::ref_type_of_v<bool>, "result"}}
        ));
        return result;
    }
    struct Fixture final
    {
        lux::test::ObjectQueue store_messages;
        sessions::SessionStore store{store_messages.dispatcherRef(), 8};
        FlowSession* session{};
        sessions::SessionId id;
        Fixture(
            FlowAuthoringSource input = source(),
            flow::FlowSourceEnvironment environment = {},
            bool bound = true,
            FlowSessionLimits limits = {}
        )
        {
            auto reservation =
                take(store.reserve<FlowSession>({"lux.editor.flowforge"}, lux::object::CodeLease::builtin()));
            id = reservation.id();
            auto candidate = take(FlowSession::create(
                id,
                bound ? sessions::SourceBinding{sessions::BoundSource{input.id, "flow.luxflow"}} : std::nullopt,
                std::move(input),
                std::move(environment),
                limits
            ));
            session = candidate.get();
            assert(store.prepare(reservation, candidate));
            assert(store.publish(reservation));
        }
        FlowEditBatch batch() const
        {
            return {session->describe().current, "flow edit", {}};
        }
        std::string encoded() const
        {
            return take(take(session->read()).encode());
        }
        editing::HistorySnapshot history() const
        {
            return take(access::FlowSessionAccess::data(*session).history->view()).snapshot;
        }
        flow::FlowSourceNode node(flow::ENodeOperation op) const
        {
            const auto snapshot = take(session->capture());
            for (const auto& node : snapshot.source().nodes)
                if (node.operation == op)
                    return node;
            std::abort();
        }
        FlowEditReceipt apply(VFlowEdit value)
        {
            auto edits = batch();
            edits.edits.push_back(std::move(value));
            return take(session->apply(std::move(edits)));
        }
    };
    struct Saved final
    {
        std::string bytes;
        sessions::SessionInfo info;
        editing::HistorySnapshot history;
        sessions::BindingRevision binding;
        std::optional<sessions::PersistedState> persisted;
        explicit Saved(const Fixture& f) : bytes(f.encoded()), info(f.session->describe()), history(f.history())
        {
            const auto& state = access::FlowSessionAccess::data(*f.session).state;
            binding = state.bindingRevision();
            persisted = state.checkpoint().persisted();
        }
        void unchanged(const Fixture& f) const
        {
            const auto now = f.session->describe();
            const auto h = f.history();
            assert(bytes == f.encoded());
            assert(info.current == now.current && info.observed == now.observed && info.dirty == now.dirty);
            assert(info.binding == now.binding && info.admission == now.admission);
            assert(history.history == h.history && history.current == h.current && history.revision == h.revision);
            assert(
                history.event_sequence == h.event_sequence && history.entry_count == h.entry_count &&
                history.cursor == h.cursor
            );
            assert(history.charged_retained_bytes == h.charged_retained_bytes && history.closed == h.closed);
            const auto& state = access::FlowSessionAccess::data(*f.session).state;
            assert(binding == state.bindingRevision() && persisted == state.checkpoint().persisted());
        }
    };
    flow::PinId dataInput(const flow::FlowSourceNode& node)
    {
        for (const auto& pin : node.inputs)
            if (pin.kind == flow::EPinKind::DATA_IN)
                return pin.id;
        std::abort();
    }
    flow::PinId dataOutput(const flow::FlowSourceNode& node)
    {
        for (const auto& pin : node.outputs)
            if (pin.kind == flow::EPinKind::DATA_OUT)
                return pin.id;
        std::abort();
    }
    flow::FlowSourceLiteral boolean(bool value)
    {
        return {flow::EFlowLiteralKind::BOOLEAN, value ? "true" : "false"};
    }
    void interaction()
    {
        Fixture f;
        auto key = take(f.store.key<FlowSession>(f.id));
        FlowInteraction gesture(f.store.access<FlowSession>(), key);
        FlowInteraction independent(f.store.access<FlowSession>(), key);
        const Saved start(f);
        const auto node = take(f.session->capture()).source().nodes.front().id;
        assert(gesture.select({node}));
        assert(independent.selection().empty());
        assert(gesture.begin("drag"));
        std::vector<VFlowEdit> preview;
        preview.push_back(FlowMoveNodes{{{node, {50, 60}}}});
        assert(gesture.preview(preview) && preview.empty());
        assert(gesture.overlay()->edits.size() == 1);
        start.unchanged(f);
        assert(gesture.cancel());
        start.unchanged(f);
        assert(gesture.begin("drag"));
        preview.push_back(FlowMoveNodes{{{node, {50, 60}}}});
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
        preview.push_back(FlowRename{"preview"});
        assert(gesture.preview(preview));
        auto concurrent = f.batch();
        concurrent.edits.push_back(FlowRename{"other view"});
        assert(f.session->apply(std::move(concurrent)));
        const Saved after(f);
        assert(!gesture.commit());
        after.unchanged(f);
        assert(gesture.synchronize() && !gesture.overlay());
        after.unchanged(f);
        auto read = take(f.session->read());
        assert(read.withRead([&](const auto&) -> FlowEditResult<void> {
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
    void selectionProvenance()
    {
        Fixture f;
        const auto key = take(f.store.key<FlowSession>(f.id));
        FlowInteraction interaction(f.store.access<FlowSession>(), key);
        const auto node = f.node(flow::ENodeOperation::BRANCH).id;
        assert(interaction.select({node}));
        const Saved before(f);
        for (int repeat{}; repeat != 1000; ++repeat)
            assert(interaction.synchronize());
        before.unchanged(f);
        auto read = take(f.session->read());
        assert(read.withRead([&]() -> FlowEditResult<void> {
            const auto denied = interaction.synchronize();
            assert(!denied && denied.error().session == sessions::ESessionError::BUSY);
            assert(interaction.selection().size() == 1 && interaction.selection().front() == node);
            return {};
        }));
        before.unchanged(f);
        f.apply(FlowRemoveNodes{{node}, {}});
        assert(interaction.synchronize() && interaction.selection().empty());
        assert(f.session->undo());
        assert(interaction.select({node}) && interaction.synchronize());
        assert(f.session->redo());
        assert(interaction.synchronize() && interaction.selection().empty());
        assert(f.session->undo() && interaction.select({node}));
        const auto current = f.session->describe().current;
        auto close = take(f.store.prepareClose(current));
        assert(f.store.close(close));
        assert(interaction.synchronize() && interaction.selection().empty());
    }
}
int main()
{
    interaction();
    selectionProvenance();
    std::puts("PASS P08 flowforge preview/cancel, one commit, persistent layout, conflict, gate, closed identity");
}
