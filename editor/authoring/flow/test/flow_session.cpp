#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/meta/Meta.hpp>
#include "../src/PreparedFlowReload.hpp"
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
        sessions::SessionStore store{8};
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
                take(store.reserve<FlowSession>({"lux.editor.flowforge"}, contracts::CodeLease::builtin()));
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
    void printPins(const char* label, const flow::FlowSourceNode& node)
    {
        std::printf("%s NodeId=%llu PinIds=", label, static_cast<unsigned long long>(node.id.value));
        for (const auto& pin : node.inputs)
            std::printf("%llu,", static_cast<unsigned long long>(pin.id.value));
        for (const auto& pin : node.outputs)
            std::printf("%llu,", static_cast<unsigned long long>(pin.id.value));
        std::puts("");
    }
    flow::FlowSourceNode capturedNode(const Fixture& f, flow::NodeId id)
    {
        const auto snapshot = take(f.session->capture());
        for (const auto& node : snapshot.source().nodes)
            if (node.id == id)
                return node;
        std::abort();
    }
    bool intersectsPins(const flow::FlowSourceNode& a, const flow::FlowSourceNode& b)
    {
        for (const auto& old : a.inputs)
        {
            for (const auto& now : b.inputs)
                if (old.id == now.id)
                    return true;
            for (const auto& now : b.outputs)
                if (old.id == now.id)
                    return true;
        }
        for (const auto& old : a.outputs)
        {
            for (const auto& now : b.inputs)
                if (old.id == now.id)
                    return true;
            for (const auto& now : b.outputs)
                if (old.id == now.id)
                    return true;
        }
        return false;
    }

    // R04-01: mixed insert -> undo -> unrelated fresh insert (not Redo).
    void idsAfterUndoBranch()
    {
        Fixture f;
        const Saved before(f);
        auto batch = f.batch();
        batch.edits.push_back(FlowRename{"first mixed transaction"});
        batch.edits.push_back(FlowInsertNode{contracts::CodeLease::builtin(), std::make_unique<flow::BranchNode>()});
        const auto applied = take(f.session->apply(std::move(batch)));
        assert(applied.inserted.nodes.size() == 1);
        const auto old = capturedNode(f, applied.inserted.nodes.front());
        const auto frozen = take(f.session->capture());
        const auto history_id = f.session->describe().current.state.history;
        assert(f.session->undo());
        assert(f.encoded() == before.bytes);

        const auto inserted =
            f.apply(FlowInsertNode{contracts::CodeLease::builtin(), std::make_unique<flow::BranchNode>()});
        assert(inserted.inserted.nodes.size() == 1);
        const auto now = capturedNode(f, inserted.inserted.nodes.front());
        printPins("issued", old);
        printPins("fresh", now);
        const bool repeated_node = old.id == now.id;
        const bool repeated_pin = intersectsPins(old, now);

        // Fresh ContentStamp but a node address remembered from the removed entity:
        // if its PinId has been recycled, this would modify the new node incorrectly.
        const Saved unchanged(f);
        auto stale_address = f.batch();
        stale_address.edits.push_back(FlowSetLiteral{dataInput(old), boolean(true)});
        const auto targeted = f.session->apply(std::move(stale_address));
        std::printf(
            "undo_branch old=%llu new=%llu repeated_node=%d repeated_pin=%d old_pin_accepted=%d\n",
            static_cast<unsigned long long>(old.id.value),
            static_cast<unsigned long long>(now.id.value),
            repeated_node,
            repeated_pin,
            bool(targeted)
        );
        std::fflush(stdout);
        assert(!repeated_node && !repeated_pin && !targeted);
        assert(now.id.value > old.id.value);
        assert(f.session->describe().current.state.history == history_id);
        unchanged.unchanged(f);
        // Frozen output is a separate historical value, not a second writable source.
        assert(flow::encodeFlowSource(frozen.source()));
    }

    // R04-02: highest node deleted -> re-materialize candidate from remaining nodes.
    // Preserving counters only at final swap is TOO LATE for this path.
    void idsWhenSeedingCandidate()
    {
        Fixture f;
        const auto issued =
            f.apply(FlowInsertNode{contracts::CodeLease::builtin(), std::make_unique<flow::BranchNode>()});
        const auto old = capturedNode(f, issued.inserted.nodes.front());
        f.apply(FlowRemoveNodes{{old.id}, {}});
        const auto history_id = f.session->describe().current.state.history;
        const Saved before(f);

        auto batch = f.batch();
        batch.edits.push_back(FlowRename{"candidate must inherit issued IDs"});
        batch.edits.push_back(FlowInsertNode{contracts::CodeLease::builtin(), std::make_unique<flow::BranchNode>()});
        const auto inserted = take(f.session->apply(std::move(batch)));
        assert(inserted.inserted.nodes.size() == 1);
        const auto now = capturedNode(f, inserted.inserted.nodes.front());
        printPins("issued", old);
        printPins("fresh", now);
        const bool repeated_node = old.id == now.id;
        const bool repeated_pin = intersectsPins(old, now);
        std::printf(
            "candidate_seed old=%llu new=%llu repeated_node=%d repeated_pin=%d\n",
            static_cast<unsigned long long>(old.id.value),
            static_cast<unsigned long long>(now.id.value),
            repeated_node,
            repeated_pin
        );
        std::fflush(stdout);
        assert(!repeated_node && !repeated_pin && now.id.value > old.id.value);
        assert(f.session->describe().current.state.history == history_id);
        assert(f.history().entry_count == before.history.entry_count + 1);
        const auto after = f.encoded();
        assert(f.session->undo() && f.encoded() == before.bytes);
        assert(f.session->redo() && f.encoded() == after);
    }

    std::vector<flow::PinId> capturedPins(const flow::FlowSource& value)
    {
        std::vector<flow::PinId> result;
        for (const auto& node : value.nodes)
        {
            for (const auto& pin : node.inputs)
                result.push_back(pin.id);
            for (const auto& pin : node.outputs)
                result.push_back(pin.id);
        }
        std::ranges::sort(result);
        assert(std::adjacent_find(result.begin(), result.end()) == result.end());
        return result;
    }
    void idsSignatureBranch()
    {
        for (const bool shrink : {false, true})
        {
            Fixture f;
            const auto def = f.node(flow::ENodeOperation::FUNC_DEF_START);
            f.apply(FlowInsertFunctionUse{def.id, false, {}});
            f.apply(FlowInsertFunctionUse{def.id, true, {}});
            const auto call = f.node(flow::ENodeOperation::GRAPH_FUNC_CALL);
            f.apply(FlowSetLiteral{dataInput(call), boolean(true)});
            f.apply(FlowConnect{dataOutput(def), dataInput(call)});
            const auto initial = take(f.session->capture());
            const auto initial_bytes = f.encoded();
            const auto stable_pins = capturedPins(initial.source());
            const auto base_signature = std::get<flow::FlowSourceSignature>(def.parameters);
            auto extended = base_signature;
            extended.arguments.push_back({"extra", "bool"});
            extended.results.push_back({"extra_result", "bool"});
            auto batch = f.batch();
            batch.edits.push_back(FlowRename{"extended"});
            batch.edits.push_back(FlowSetSignature{def.id, def.name, extended});
            take(f.session->apply(std::move(batch)));
            const auto first = take(f.session->capture());
            const auto first_bytes = f.encoded();
            const auto issued = capturedPins(first.source());
            if (shrink)
            {
                batch = f.batch();
                batch.edits.push_back(FlowRename{initial.source().name});
                batch.edits.push_back(FlowSetSignature{def.id, def.name, base_signature});
                take(f.session->apply(std::move(batch)));
            }
            else
            {
                assert(f.session->undo() && f.encoded() == initial_bytes);
                assert(f.session->redo() && f.encoded() == first_bytes);
                assert(f.session->undo());
            }
            assert(f.encoded() == initial_bytes);
            extended.arguments.back().name = "other";
            batch = f.batch();
            batch.edits.push_back(FlowRename{"new branch"});
            batch.edits.push_back(FlowSetSignature{def.id, def.name, extended});
            take(f.session->apply(std::move(batch)));
            const auto second = take(f.session->capture());
            const auto now = capturedPins(second.source());
            assert(second.source().nodes.size() == initial.source().nodes.size());
            for (std::size_t i = 0; i < second.source().nodes.size(); ++i)
                assert(second.source().nodes[i].id == initial.source().nodes[i].id);
            for (const auto pin : stable_pins)
                assert(std::ranges::binary_search(now, pin));
            std::size_t fresh_count{};
            for (const auto pin : now)
                if (!std::ranges::binary_search(stable_pins, pin))
                {
                    ++fresh_count;
                    assert(!std::ranges::binary_search(issued, pin));
                }
            assert(fresh_count > 0 && second.source().links == initial.source().links);
            const auto second_bytes = f.encoded();
            assert(f.session->undo() && f.encoded() == initial_bytes);
            assert(f.session->redo() && f.encoded() == second_bytes);
        }
        std::puts("R04-03 PASS: signature Undo/shrink branch retains old pins, allocates new pins without reuse, "
                  "stable nodes/links/literals");
    }
    void idsRestore()
    {
        Fixture f;
        auto batch = f.batch();
        batch.edits.push_back(FlowRename{"restore"});
        batch.edits.push_back(FlowInsertNode{contracts::CodeLease::builtin(), std::make_unique<flow::BranchNode>()});
        const auto receipt = take(f.session->apply(std::move(batch)));
        const auto old = capturedNode(f, receipt.inserted.nodes.front());
        const auto after = f.encoded();
        assert(f.session->undo());
        assert(f.session->redo() && f.encoded() == after);
        assert(capturedNode(f, old.id) == old);
        f.apply(FlowRemoveNodes{{old.id}, {}});
        auto restored = std::make_unique<flow::BranchNode>(old.id.value);
        for (std::size_t i = 0; i < restored->inPins().size(); ++i)
            assert(flow::FlowGraph::assignDetachedPinId(*restored->inPins()[i], old.inputs[i].id));
        for (std::size_t i = 0; i < restored->outPins().size(); ++i)
            assert(flow::FlowGraph::assignDetachedPinId(*restored->outPins()[i], old.outputs[i].id));
        const auto restored_id =
            f.apply(FlowInsertNode{contracts::CodeLease::builtin(), std::move(restored), old.layout, true});
        assert(restored_id.inserted.nodes.front() == old.id && f.encoded() == after);
        assert(f.session->undo());
        assert(f.session->redo() && f.encoded() == after);
        f.apply(FlowRemoveNodes{{old.id}, {}});
        const auto fresh =
            f.apply(FlowInsertNode{contracts::CodeLease::builtin(), std::make_unique<flow::BranchNode>()});
        const auto now = capturedNode(f, fresh.inserted.nodes.front());
        assert(now.id.value > old.id.value && !intersectsPins(old, now));
        // The create copy boundary also keeps issued IDs absent from the input's live records.
        auto input = source();
        const auto index = input.graph.addNodes(std::make_unique<flow::BranchNode>());
        const auto node = input.graph.getNode(index).node->id();
        const auto captured = take(flow::captureFlowSource(input.id, input.name, input.graph));
        const auto erased = *std::ranges::find(captured.nodes, node, &flow::FlowSourceNode::id);
        assert(input.graph.removeNode(index));
        Fixture copied(std::move(input));
        const auto next =
            copied.apply(FlowInsertNode{contracts::CodeLease::builtin(), std::make_unique<flow::BranchNode>()});
        const auto new_node = capturedNode(copied, next.inserted.nodes.front());
        assert(new_node.id.value > erased.id.value && !intersectsPins(erased, new_node));
        std::puts("R04-04 PASS: Redo/preserve_ids restore exact bytes and identities; future allocation and create "
                  "copy retain watermarks");
    }
    void idsFailure()
    {
        Fixture f;
        const Saved initial(f);
        for (const bool invalid_variable : {true, false})
        {
            auto batch = f.batch();
            batch.edits.push_back(FlowInsertNode{contracts::CodeLease::builtin(), std::make_unique<flow::BranchNode>()}
            );
            if (invalid_variable)
                batch.edits.push_back(FlowRemoveVariable{UINT64_MAX - 1});
            else
                batch.edits.push_back(FlowConnect{{UINT64_MAX}, {UINT64_MAX - 1}});
            assert(!f.session->apply(std::move(batch)));
            initial.unchanged(f);
        }
        auto batch = f.batch();
        const flow::NodeId unpublished{UINT64_MAX / 2};
        batch.edits.push_back(FlowInsertNode{
            contracts::CodeLease::builtin(),
            std::make_unique<flow::BranchNode>(unpublished.value),
            {},
            true
        });
        batch.edits.push_back(FlowRemoveNodes{{unpublished}, {}});
        assert(take(f.session->apply(std::move(batch))).effect == editing::EEditEffect::NO_CHANGE);
        initial.unchanged(f);
        const auto next =
            f.apply(FlowInsertNode{contracts::CodeLease::builtin(), std::make_unique<flow::BranchNode>()});
        assert(next.inserted.nodes.front().value < unpublished.value);
        assert(f.session->undo() && f.encoded() == initial.bytes);
        assert(f.session->redo());
        FlowSessionLimits limits;
        limits.history.max_staging_bytes = 1;
        Fixture tiny(source(), {}, true, limits);
        const Saved unchanged(tiny);
        batch = tiny.batch();
        batch.edits.push_back(FlowInsertNode{contracts::CodeLease::builtin(), std::make_unique<flow::BranchNode>()});
        batch.edits.push_back(FlowRename{"too large"});
        assert(!tiny.session->apply(std::move(batch)));
        unchanged.unchanged(tiny);
        std::puts("R04-05 PASS: failed candidates/NO_CHANGE/budget preserve full "
                  "source/history/observed/dirty/binding/checkpoint; unpublished IDs discarded");
    }
    void idsExhaustion()
    {
        namespace g = lux::graph;
        const g::NodeTypeId type{1};
        const g::PinSemanticId semantic{1};
        const auto output = g::EPinDirection::OUTPUT;
        const auto add_pin = [&](g::GraphTopology& graph, g::NodeId node) {
            return graph.addPin(node, output, g::kUnlimitedFan, semantic);
        };
        const auto exhausted = [](const auto& value) {
            assert(!value && value.error().code == g::EGraphTopologyError::ID_EXHAUSTED);
        };
        g::GraphTopology issued;
        assert(issued.insertNode({{100}, type}));
        assert(issued.insertPin({{1000}, {100}, output, g::kUnlimitedFan, semantic}));
        auto removed = take(issued.detachNode({100}));
        g::GraphTopology live;
        const auto node = take(live.addNode(type));
        const auto pin = take(add_pin(live, node));
        const auto record = *live.findNode(node);
        const auto pin_record = *live.findPin(pin);
        live.preserveIssuedIdsFrom(issued);
        issued.preserveIssuedIdsFrom(live);
        live.preserveIssuedIdsFrom(live);
        assert(*live.findNode(node) == record && *live.findPin(pin) == pin_record);
        assert(take(live.addNode(type)).value == 101);
        assert(take(add_pin(live, node)).value == 1001);
        assert(issued.restoreNode(std::move(removed)));
        assert(take(issued.addNode(type)).value == 101);
        assert(take(add_pin(issued, {100})).value == 1001);
        g::GraphTopology spent;
        assert(spent.insertNode({{UINT64_MAX}, type}));
        assert(spent.insertPin({{UINT64_MAX}, {UINT64_MAX}, output, g::kUnlimitedFan, semantic}));
        auto max_record = take(spent.detachNode({UINT64_MAX}));
        for (const bool reverse : {false, true})
        {
            g::GraphTopology a;
            const auto low = take(a.addNode(type));
            const auto low_pin = take(add_pin(a, low));
            auto low_record = take(a.detachNode(low));
            auto b = spent;
            if (reverse)
                b.preserveIssuedIdsFrom(a);
            a.preserveIssuedIdsFrom(b);
            b.preserveIssuedIdsFrom(a);
            a.preserveIssuedIdsFrom(a);
            assert(a.restoreNode(std::move(low_record)));
            auto restore_pin = take(a.detachPin(low_pin));
            assert(a.restorePin(std::move(restore_pin)));
            assert(a.findNode(low) && a.findPin(low_pin));
            exhausted(a.addNode(type));
            exhausted(add_pin(a, low));
            exhausted(b.addNode(type));
            exhausted(add_pin(b, low));
        }
        assert(spent.restoreNode(std::move(max_record)));
        assert(spent.insertNode({{2}, type}));
        assert(spent.insertPin({{2}, {2}, output, g::kUnlimitedFan, semantic}));
        exhausted(spent.addNode(type));
        exhausted(add_pin(spent, {2}));
        // Each exhaustion bit is independent of the other identity domain.
        g::GraphTopology pin_only;
        assert(pin_only.insertNode({{1}, type}));
        assert(pin_only.insertPin({{UINT64_MAX}, {1}, output, g::kUnlimitedFan, semantic}));
        g::GraphTopology pin_target;
        pin_target.preserveIssuedIdsFrom(pin_only);
        const auto next_node = take(pin_target.addNode(type));
        assert(next_node.value == 2);
        exhausted(add_pin(pin_target, next_node));
        g::GraphTopology node_only;
        assert(node_only.insertNode({{UINT64_MAX}, type}));
        g::GraphTopology node_target;
        assert(node_target.insertNode({{1}, type}));
        node_target.preserveIssuedIdsFrom(node_only);
        exhausted(node_target.addNode(type));
        assert(take(add_pin(node_target, {1})).value == 1);
        flow::FlowGraph variable_source, target;
        assert(variable_source.addVariableWithId(UINT64_MAX - 1, "last", &meta::ref_type_of_v<bool>, {}));
        assert(variable_source.removeVariable(UINT64_MAX - 1));
        variable_source.topology().preserveIssuedIdsFrom(spent);
        target.preserveIssuedIdsFrom(variable_source);
        variable_source.preserveIssuedIdsFrom(target);
        target.preserveIssuedIdsFrom(target);
        assert(target.nextVariableId() == UINT64_MAX);
        assert(target.addVariable("no more", &meta::ref_type_of_v<bool>, {}) == 0);
        assert(target.addVariableWithId(1, "restored", &meta::ref_type_of_v<bool>, {}));
        assert(target.nextVariableId() == UINT64_MAX && target.findVariable(1));
        exhausted(target.topology().addNode(type));
        exhausted(add_pin(target.topology(), {1}));
        std::puts("R04-06 PASS: actual GraphTopology and FlowGraph merge, self/bidirectional, absorbing node/pin "
                  "exhaustion, explicit restore, variables");
    }

    void content()
    {
        Fixture f;
        Saved original(f);
        const auto branch = f.node(flow::ENodeOperation::BRANCH);
        const auto definition = f.node(flow::ENodeOperation::FUNC_DEF_START);
        auto batch = f.batch();
        batch.edits.push_back(FlowRename{"edited flow"});
        batch.edits.push_back(FlowSetLiteral{dataInput(branch), boolean(true)});
        batch.edits.push_back(FlowAddVariable{"enabled", "bool", boolean(true)});
        batch.edits.push_back(FlowMoveNodes{{{branch.id, {42, 24}}}});
        batch.edits.push_back(FlowInsertFunctionUse{definition.id, false, {4, 5}});
        batch.edits.push_back(FlowInsertFunctionUse{definition.id, true, {6, 7}});
        auto changed = take(f.session->apply(std::move(batch)));
        assert(changed.inserted.nodes.size() == 2 && changed.inserted.variables.size() == 1);
        assert(f.history().entry_count == 1 && f.session->describe().dirty);
        auto frozen = take(f.session->capture());
        assert(frozen.source().variables.front().name == "enabled");
        const auto call = f.node(flow::ENodeOperation::GRAPH_FUNC_CALL);
        f.apply(FlowConnect{dataOutput(definition), dataInput(call)});
        const auto connected = f.encoded();
        Saved no_change(f);
        assert(f.apply(FlowConnect{dataOutput(definition), dataInput(call)}).effect == editing::EEditEffect::NO_CHANGE);
        no_change.unchanged(f);
        f.apply(FlowDisconnect{dataOutput(definition), dataInput(call)});
        assert(f.session->undo());
        assert(f.encoded() == connected);
        assert(f.session->undo());
        assert(f.session->undo());
        assert(f.encoded() == original.bytes && !f.session->describe().dirty);
        assert(f.session->redo());
        assert(f.session->redo());
        assert(f.encoded() == connected);
        assert(frozen.source().links.empty() && frozen.source().name == "edited flow");
        std::vector<flow::ExportMethodNode> exports{
            {flow::FlowForgeExportNodeId{1}, f.node(flow::ENodeOperation::ON_EVENT).id, 5678}
        };
        f.apply(FlowSetExports{std::move(exports)});
        assert(take(f.session->capture()).source().exports.front().symbol == 5678);
        assert(f.session->undo());
        assert(f.encoded() == connected);
        // Branch after undo cannot recycle a variable identity issued by an atomic batch.
        assert(f.session->undo());
        assert(f.session->undo());
        auto next = f.apply(FlowAddVariable{"later", "bool", boolean(false)}).inserted.variables.front();
        assert(next > changed.inserted.variables.front());
        Saved before_failure(f);
        auto bad = f.batch();
        bad.edits.push_back(FlowRename{"not published"});
        bad.edits.push_back(FlowRemoveVariable{999});
        assert(!f.session->apply(std::move(bad)));
        before_failure.unchanged(f);
        assert(!f.session->capture({1}));
        before_failure.unchanged(f);
        std::puts("X04-01: actual Flow graph/variables/signatures/exports/links/layout/history without linker PASS");
    }
    void failure()
    {
        Fixture f;
        const auto variable = f.apply(FlowAddVariable{"condition", "bool", boolean(true)}).inserted.variables.front();
        f.apply(FlowInsertNode{
            contracts::CodeLease::builtin(),
            std::make_unique<flow::GetVariableNode>(
                variable,
                flow::DataPinInfo{"condition", &meta::ref_type_of_v<bool>}
            )
        });
        Saved referenced(f);
        auto batch = f.batch();
        batch.edits.push_back(FlowRemoveVariable{variable});
        assert(!f.session->apply(std::move(batch)));
        referenced.unchanged(f);
        batch = f.batch();
        batch.edits.push_back(FlowSetVariable{{variable, "condition", "double", {flow::EFlowLiteralKind::REAL, "2"}}});
        assert(!f.session->apply(std::move(batch)));
        referenced.unchanged(f);
        const auto def = f.node(flow::ENodeOperation::FUNC_DEF_START);
        f.apply(FlowInsertFunctionUse{def.id, false, {}});
        f.apply(FlowInsertFunctionUse{def.id, true, {}});
        const auto call = f.node(flow::ENodeOperation::GRAPH_FUNC_CALL);
        f.apply(FlowConnect{dataOutput(def), dataInput(call)});
        Saved signature(f);
        batch = f.batch();
        batch.edits.push_back(FlowSetSignature{def.id, "broken", {}});
        assert(!f.session->apply(std::move(batch)));
        signature.unchanged(f);
        auto layout = f.node(flow::ENodeOperation::ON_EVENT).id;
        batch = f.batch();
        batch.edits.push_back(FlowRemoveNodes{{layout}, {}});
        assert(!f.session->apply(std::move(batch)));
        signature.unchanged(f);
        auto value = std::get<flow::FlowSourceSignature>(def.parameters);
        value.arguments.front().name = "newName";
        f.apply(FlowSetSignature{def.id, "updated", value});
        assert(dataInput(f.node(flow::ENodeOperation::GRAPH_FUNC_CALL)) == dataInput(call));
        assert(f.session->undo());
        assert(f.encoded() == signature.bytes);
        std::puts("X04-02: referenced variable/signature/exports reject with entire state unchanged PASS");
    }
    void mixedSignature()
    {
        Fixture f;
        const auto def = f.node(flow::ENodeOperation::FUNC_DEF_START);
        f.apply(FlowInsertFunctionUse{def.id, false, {}});
        const auto call = f.node(flow::ENodeOperation::GRAPH_FUNC_CALL);
        const auto pin = dataInput(call);
        Saved prior(f);
        const auto before_entries = f.history().entry_count;
        auto batch = f.batch();
        batch.edits.push_back(FlowSetLiteral{pin, boolean(true)});
        auto signature = std::get<flow::FlowSourceSignature>(def.parameters);
        signature.arguments.front().name = "replaced";
        batch.edits.push_back(FlowSetSignature{def.id, "replacement", signature});
        batch.edits.push_back(FlowSetLiteral{pin, boolean(false)});
        assert(f.session->apply(std::move(batch)));
        assert(f.history().entry_count == before_entries + 1);
        auto now = f.node(flow::ENodeOperation::GRAPH_FUNC_CALL);
        for (const auto& input : now.inputs)
            if (input.id == pin)
                assert(input.literal == boolean(false));
        const auto after = f.encoded();
        assert(f.session->undo());
        assert(f.encoded() == prior.bytes);
        assert(f.session->redo());
        assert(f.encoded() == after);
        std::puts("ordered literal/signature replacement/literal and atomic replay PASS");
    }

    void mixedRecreate()
    {
        Fixture f;
        const auto branch = f.node(flow::ENodeOperation::BRANCH);
        const auto pin = dataInput(branch);
        Saved saved(f);
        auto replacement = std::make_unique<flow::BranchNode>(branch.id.value);
        for (std::size_t i = 0; i < replacement->inPins().size(); ++i)
            assert(flow::FlowGraph::assignDetachedPinId(*replacement->inPins()[i], branch.inputs[i].id));
        for (std::size_t i = 0; i < replacement->outPins().size(); ++i)
            assert(flow::FlowGraph::assignDetachedPinId(*replacement->outPins()[i], branch.outputs[i].id));
        auto* input = static_cast<flow::DataInPin*>(replacement->inPins().back());
        assert(input->kind() == flow::EPinKind::DATA_IN);
        assert(input->setConstantData(take(flow::materializeFlowLiteral(boolean(false), *input->info().type))));
        auto batch = f.batch();
        batch.edits.push_back(FlowSetLiteral{pin, boolean(true)});
        batch.edits.push_back(FlowRemoveNodes{{branch.id}, {}});
        batch.edits.push_back(FlowInsertNode{contracts::CodeLease::builtin(), std::move(replacement), {9, 10}, true});
        assert(f.session->apply(std::move(batch)));
        const auto node = f.node(flow::ENodeOperation::BRANCH);
        for (const auto& value : node.inputs)
            if (value.id == pin)
                assert(value.literal == boolean(false));
        assert(f.history().entry_count == 1);
        const auto after = f.encoded();
        assert(f.session->undo());
        assert(f.encoded() == saved.bytes);
        assert(f.session->redo());
        assert(f.encoded() == after);
        std::puts("ordered literal/delete/recreate same IDs: old scratch never overwrites new payload PASS");
    }
    struct Lifetime final
    {
        int alive{}, destroyed{}, callbacks{};
        bool released{}, premature{};
    };
    struct NativeRecord final
    {
        std::int32_t value{};
    };
    struct Metadata final
    {
        Lifetime& stats;
        meta::RefClass record;
        meta::RefFunction function;
        std::array<const meta::RefClass*, 1> classes;
        std::array<const meta::RefFunction*, 1> functions;
        explicit Metadata(Lifetime& value) : stats(value)
        {
            record.name = "NativeRecord";
            record.full_name = "NativeRecord";
            record.type = meta::ref_type_of_v<NativeRecord>;
            record.type.name = record.full_name;
            record.type.ptr = &record;
            record.fields.push_back({"value", meta::ref_type_of_v<std::int32_t>});
            function.invokable.name = "nativeProbe";
            function.invokable.full_name = "nativeProbe";
            function.invokable.type_signature = "int32_t()";
            function.invokable.return_type = meta::ref_type_of_v<std::int32_t>;
            classes = {&record};
            functions = {&function};
        }
        ~Metadata()
        {
            stats.premature = stats.alive != 0;
            stats.released = true;
        }
    };
    flow::FlowSourceEnvironment environment(const std::shared_ptr<Metadata>& owner)
    {
        flow::FlowSourceEnvironment result;
        result.classes = owner->classes;
        result.functions = owner->functions;
        result.code_lifetime = owner;
        return result;
    }
    class ProbeNode final : public flow::BranchNode
    {
    public:
        ProbeNode(Lifetime& stats, std::weak_ptr<Metadata> metadata, std::function<void()> callback = {})
            : stats_(stats), metadata_(std::move(metadata)), callback_(std::move(callback))
        {
            ++stats_.alive;
        }
        ~ProbeNode() override
        {
            assert(!metadata_.expired());
            auto owner = metadata_.lock();
            assert(owner->classes.front()->fields.front().name == "value");
            if (callback_)
            {
                ++stats_.callbacks;
                callback_();
            }
            --stats_.alive;
            ++stats_.destroyed;
        }

    private:
        Lifetime& stats_;
        std::weak_ptr<Metadata> metadata_;
        std::function<void()> callback_;
    };
    void lifetime()
    {
        Lifetime stats;
        auto owner = std::make_shared<Metadata>(stats);
        std::weak_ptr<Metadata> weak = owner;
        auto input = source();
        (void)input.graph.addNodes(std::make_unique<flow::GetFieldNode>(owner->record, owner->record.fields.front()));
        (void)input.graph.addNodes(std::make_unique<flow::NativeFuncCall>(owner->function));
        auto fixture = std::make_unique<Fixture>(std::move(input), environment(owner));
        const auto variable =
            fixture->apply(FlowAddVariable{"native", "NativeRecord", {flow::EFlowLiteralKind::ZERO, {}}}
            ).inserted.variables.front();
        fixture->apply(FlowSetVariable{{variable, "renamed native", "NativeRecord", {flow::EFlowLiteralKind::ZERO, {}}}}
        );
        fixture->apply(FlowInsertNode{contracts::CodeLease::plugin(owner), std::make_unique<ProbeNode>(stats, owner)});
        auto frozen = take(fixture->session->capture());
        owner.reset();
        assert(!weak.expired());
        assert(fixture->session->undo()); // custom node parked in history, metadata arrays have no external owner
        assert(stats.alive == 1);
        assert(fixture->session->redo());
        assert(fixture->session->undo());
        fixture.reset();
        assert(stats.alive == 0 && stats.destroyed == 1 && stats.released && !stats.premature);
        assert(weak.expired()); // frozen data contains only owned values, safely usable after code/metadata release
        assert(flow::encodeFlowSource(frozen.source()));
        std::puts("X04-03: owned metadata spans/descriptors and last code owner outlive live/parked nodes PASS");
    }
    void reading()
    {
        Fixture f;
        Saved saved(f);
        auto view = take(f.session->read());
        const auto test = [&] {
            auto batch = f.batch();
            batch.edits.push_back(FlowRename{"reentrant"});
            auto result = f.session->apply(std::move(batch));
            assert(!result && result.error().session == sessions::ESessionError::BUSY);
            assert(!f.session->undo());
            assert(!f.session->capture());
            assert(!f.store.prepareClose(saved.info.current));
        };
        assert(view.withRead([&](const auto& source) -> FlowEditResult<void> {
            test();
            assert(source.name == "flow source");
            struct Cleanup
            {
                const std::function<void()>& action;
                ~Cleanup()
                {
                    action();
                }
            };
            std::function<void()> callback = test;
            Cleanup cleanup{callback};
            auto graph = take(flow::materializeFlowSource(source));
            assert(!graph.nodes().empty());
            return {};
        }));
        saved.unchanged(f);
        try
        {
            (void)view.withRead([&](const auto&) -> FlowEditResult<void> {
                struct Cleanup
                {
                    const std::function<void()>& action;
                    ~Cleanup()
                    {
                        action();
                    }
                };
                std::function<void()> callback = test;
                Cleanup cleanup{callback};
                throw std::runtime_error("foreign codec failure");
            });
            assert(false);
        }
        catch (const std::runtime_error&)
        {}
        saved.unchanged(f);
        f.apply(FlowRename{"after read"});
        std::puts("READING covers callback, temporary destruction and unwind; normal edit resumes PASS");
    }
    void inputCleanup(std::string_view scenario)
    {
        const bool external_owner = scenario == "reload-unbound-external";
        const bool bound = scenario != "reload-unbound" && !external_owner;
        Fixture f(source(), {}, bound);
        Saved saved(f);
        Lifetime stats;
        auto owner = std::make_shared<Metadata>(stats);
        bool blocked{};
        auto input = source();
        (void)input.graph.addNodes(std::make_unique<ProbeNode>(stats, owner, [&] {
            auto edit = f.batch();
            edit.edits.push_back(FlowRename{"bad reentry"});
            const auto attempted = f.session->apply(std::move(edit));
            blocked = !attempted && attempted.error().session == sessions::ESessionError::BUSY;
        }));
        auto env = environment(owner);
        if (!external_owner)
            owner.reset();
        const auto reject = [&]() -> FlowEditResult<void> {
            auto prepared = access::PreparedFlowReload::prepare(*f.session, std::move(input), std::move(env));
            assert(!prepared);
            return {};
        };
        if (scenario == "reload-reading")
        {
            assert(take(f.session->read()).withRead([&](const auto&) -> FlowEditResult<void> {
                assert(reject());
                assert(f.session->describe().admission == sessions::EEditAdmission::READING);
                return {};
            }));
        }
        else if (scenario == "reload-closing")
        {
            auto permit = take(f.store.prepareClose(saved.info.current));
            assert(reject());
            assert(f.session->describe().admission == sessions::EEditAdmission::CLOSING);
        }
        else
        {
            if (scenario == "reload-identity")
                input.id = asset::AssetId{};
            assert(reject());
        }
        assert(blocked && stats.destroyed == 1 && stats.callbacks == 1 && stats.alive == 0);
        if (external_owner)
        {
            assert(!stats.released);
            owner.reset();
        }
        assert(stats.released && !stats.premature);
        saved.unchanged(f);
        f.apply(FlowRename{"normal after rejection"});
        std::puts("owned reload input: last lease, destruction admission, full unchanged state PASS");
    }

    void editInputCleanup(std::string_view scenario)
    {
        Fixture f;
        Saved saved(f);
        Lifetime stats;
        auto owner = std::make_shared<Metadata>(stats);
        bool blocked{};
        auto input = std::make_unique<ProbeNode>(stats, owner, [&] {
            auto nested = f.batch();
            nested.edits.push_back(FlowRename{"reentrant input"});
            const auto result = f.session->apply(std::move(nested));
            blocked = !result && result.error().session == sessions::ESessionError::BUSY;
        });
        auto batch = f.batch();
        batch.edits.push_back(FlowInsertNode{contracts::CodeLease::plugin(owner), std::move(input)});
        owner.reset();
        const auto reject = [&]() -> FlowEditResult<void> {
            const auto result = f.session->apply(std::move(batch));
            assert(!result);
            return {};
        };
        if (scenario == "input-reading")
            assert(take(f.session->read()).withRead([&](const auto&) -> FlowEditResult<void> { return reject(); }));
        else if (scenario == "input-closing")
        {
            auto permit = take(f.store.prepareClose(saved.info.current));
            assert(reject());
            assert(f.session->describe().admission == sessions::EEditAdmission::CLOSING);
        }
        else
        {
            batch.expected.state.serial += 100;
            assert(reject());
        }
        assert(blocked && stats.destroyed == 1 && stats.alive == 0 && stats.released && !stats.premature);
        saved.unchanged(f);
        f.apply(FlowRename{"after rejected input"});
        std::puts("consumed edit input cleanup stays gated, last code owner releases after node PASS");
    }
    void budgets()
    {
        Fixture f;
        Saved before(f);
        auto bad = f.batch();
        bad.edits.push_back(FlowSetLiteral{
            dataInput(f.node(flow::ENodeOperation::BRANCH)),
            {flow::EFlowLiteralKind::BOOLEAN, "not-a-bool"}
        });
        assert(!f.session->apply(std::move(bad)));
        before.unchanged(f);
        FlowSessionLimits limits;
        limits.history.max_staging_bytes = 1;
        Fixture tiny(source(), {}, true, limits);
        Saved saved(tiny);
        auto batch = tiny.batch();
        batch.edits.push_back(FlowRename{"cannot fit"});
        auto result = tiny.session->apply(std::move(batch));
        assert(!result && result.error().history.code == editing::EEditError::STAGING_LIMIT);
        saved.unchanged(tiny);
        batch = tiny.batch();
        batch.edits.push_back(FlowRename{"atomic rejection"});
        batch.edits.push_back(FlowAddVariable{"flag", "bool", boolean(true)});
        assert(!tiny.session->apply(std::move(batch)));
        saved.unchanged(tiny);
        std::puts("invalid literal/local and structural staging limits preserve full state PASS");
    }
    void reloadSuccess()
    {
        Fixture f;
        Saved before(f);
        auto replacement = source();
        replacement.name = "reloaded";
        auto prepared = take(access::PreparedFlowReload::prepare(*f.session, std::move(replacement)));
        before.unchanged(f);
        assert(prepared.adopt(*f.session));
        assert(!f.session->describe().dirty && f.history().entry_count == 0);
        assert(take(f.session->capture()).source().name == "reloaded");
        prepared = take(access::PreparedFlowReload::prepare(*f.session, source()));
        f.apply(FlowRename{"newer"});
        Saved newer(f);
        assert(!prepared.adopt(*f.session));
        newer.unchanged(f);
        std::puts("reload success and stale adoption PASS");
    }
}
int main(int argc, char** argv)
{
    const std::string_view name = argc > 1 ? argv[1] : "content";
    if (name == "ids-undo-branch")
        idsAfterUndoBranch();
    else if (name == "ids-candidate-seed")
        idsWhenSeedingCandidate();
    else if (name == "ids-signature")
        idsSignatureBranch();
    else if (name == "ids-restore")
        idsRestore();
    else if (name == "ids-failure")
        idsFailure();
    else if (name == "ids-exhaustion")
        idsExhaustion();
    else if (name == "content")
        content();
    else if (name == "failure")
        failure();
    else if (name == "mixed-signature")
        mixedSignature();
    else if (name == "mixed-recreate")
        mixedRecreate();
    else if (name == "lifetime")
        lifetime();
    else if (name == "reading")
        reading();
    else if (name == "reload-success")
        reloadSuccess();
    else if (name == "budgets")
        budgets();
    else if (name.starts_with("input-"))
        editInputCleanup(name);
    else if (name.starts_with("reload-"))
        inputCleanup(name);
    else
        return 2;
}
