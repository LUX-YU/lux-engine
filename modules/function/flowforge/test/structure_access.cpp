#include "FlowTest.hpp"
#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>

#include <array>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <utility>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    template <typename T>
    concept Rekeyable = requires(T& node) { node.assignStableId(NodeId{2}); };
    template <typename T>
    concept Rebindable = requires(T& node) { node.assignGraph(nullptr); };
    template <typename T>
    concept MutableTopology = requires(T& graph) { graph.topology().detachPin(PinId{1}); };
    template <typename T>
    concept MutableInputs = requires(T& node) { node.inPins().clear(); };
    template <typename T>
    concept MutableOutputs = requires(T& node) { node.outPins().clear(); };
    template <typename T>
    concept MutableOwner = requires(T& graph) { graph.findNodeById(NodeId{1}).reset(); };
    template <typename T>
    concept Unregisterable = requires(T& graph, FlowPinPayload& pin) { graph.unregisterPin(pin); };
    template <typename T>
    concept EmbeddedIdentity = requires(const T& node) { node.id(); };

    template <typename T>
    concept LinkablePin = requires(T& pin, T* other) { pin.linkTo(other); };
    template <typename T>
    concept UnlinkablePin = requires(T& pin, T* other) { pin.unlinkFrom(other); };
    template <typename T>
    concept PinPreflight = requires(const T& pin, T* other) { pin.canLink(other); };

    template <typename T>
    concept PinLinks = requires(const T& pin) { pin.linkedPins(); };
    template <typename T>
    concept PinSuccessor = requires(const T& pin) { pin.nextPin(); };
    template <typename T>
    concept PinSource = requires(const T& pin) { pin.linkedPin(); };
    template <typename T>
    concept PinConsumers = requires(const T& pin) { pin.linkPins(); };

    template <typename T>
    concept DynamicPinMutation = requires(T& node) { node.addExecOutPin(); };
    template <typename T>
    concept Reconstructable = requires(T& node) { node.reconstruct(); };

    template <typename T>
    concept PointerConnect =
        requires(T& graph, FlowPinPayload& first, FlowPinPayload& second) { graph.connect(first, second); };

    template <typename T>
    concept PointerDisconnect =
        requires(T& graph, FlowPinPayload& first, FlowPinPayload& second) { graph.disconnect(first, second); };

    template <typename T>
    concept BackReference = requires(const T& node) { node.graph(); };

    template <typename T>
    concept PinOwner = requires(const T& pin) { pin.node(); };

    static_assert(!DynamicPinMutation<FlowNode> && !Reconstructable<FlowNode> && !Reconstructable<NativeCallPayload>);
    static_assert(!PinLinks<FlowPinPayload> && !PinSuccessor<FlowPinPayload>);
    static_assert(!PinSource<FlowPinPayload> && !PinConsumers<FlowPinPayload>);
    static_assert(!LinkablePin<FlowPinPayload> && !UnlinkablePin<FlowPinPayload> && !PinPreflight<FlowPinPayload>);
    static_assert(!Rekeyable<FlowNode> && !Rebindable<FlowNode> && !BackReference<FlowNode>);
    static_assert(!MutableTopology<FlowGraph> && !MutableOwner<FlowGraph> && !Unregisterable<FlowGraph>);
    static_assert(!MutableInputs<FlowNode> && !MutableOutputs<FlowNode>);
    static_assert(!EmbeddedIdentity<FlowNode> && !EmbeddedIdentity<FlowPinPayload> && !PinOwner<FlowPinPayload>);
    static_assert(!std::is_constructible_v<FlowNode, std::uint64_t>);
    static_assert(!std::is_constructible_v<FunctionCallPayload, const FunctionPayload&>);
    static_assert(!std::is_constructible_v<FunctionReturnPayload, const FunctionPayload&>);
    static_assert(!PointerConnect<FlowGraph> && !PointerDisconnect<FlowGraph>);
    static_assert(std::is_same_v<decltype(std::declval<FlowGraph&>().node(NodeId{})), const FlowNode*>);

    void require(bool value, std::source_location where = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "Flow structure contract failed at %u\n", where.line());
            std::abort();
        }
    }

    void check(const FlowGraph& graph)
    {
        require(graph.nodes().size() == graph.topology().nodes().size());
        std::size_t pin_count{};
        for (const auto& [id, node] : graph.nodes())
        {
            require(graph.topology().findNode(id) != nullptr);
            require(graph.node(id) == node);
            const auto schema = node->definition->describePins(node->payload);
            require(schema.has_value());
            for (const auto& declaration : *schema)
            {
                const auto pin = graph.pinId(id, declaration.semantic);
                const auto* record = graph.topology().findPin(pin);
                require(record != nullptr);
                require(record->owner == id && record->direction == declaration.direction);
                require(graph.pin(pin) != nullptr && graph.pin(pin)->role == declaration.role);
                ++pin_count;
            }
        }
        require(pin_count == graph.topology().pins().size());
        for (const auto& link : graph.topology().links())
        {
            require(graph.pin(link.from) != nullptr && graph.pin(link.to) != nullptr);
        }
    }

    std::string sourceBytes(const FlowGraph& graph)
    {
        const asset::AssetId asset{std::array<std::uint8_t, 16>{1}};
        auto source = captureFlowSource(asset, "structure", graph);
        require(source.has_value());
        auto encoded = encodeFlowSource(*source);
        require(encoded.has_value());
        return std::move(*encoded);
    }

    FlowNodeSnapshot replaceSequence(
        FlowGraph& graph,
        NodeId id,
        std::size_t extra,
        std::span<const PinId> pins,
        std::span<const graph::LinkRecord> links = {}
    )
    {
        auto candidate = flow_test::sequenceSnapshot(id, extra, pins);
        const auto* original = graph.node(id);
        candidate.value.name = original->name;
        candidate.value.creator = original->creator;
        const auto before = sourceBytes(graph);
        const std::array erase{id};
        const std::array<FlowNodeEntry, 1> insert{{{id, &candidate.value, candidate.pins}}};
        std::vector<graph::GraphLayoutEntry> layout;
        if (const auto* saved = graph.layout().find(id))
        {
            layout.push_back({id, *saved});
        }
        auto plan =
            FlowGraphEdit::prepare(graph, {.insert = insert, .erase = erase, .connect = links, .place = layout});
        require(plan.has_value() && candidate.value.definition && sourceBytes(graph) == before);
        const auto old_extra = original->payload.get<SequencePayload>()->additional_outputs;
        plan->commit();
        auto removed = plan->takeRemoved();
        require(removed.size() == 1 && removed.front().id == id);
        require(removed.front().value.payload.get<SequencePayload>()->additional_outputs == old_extra);
        require(graph.node(id)->payload.get<SequencePayload>()->additional_outputs == extra);
        return std::move(removed.front());
    }

    void checkReceiver()
    {
        FlowGraph target, foreign;
        const auto populate = [](FlowGraph& graph)
        {
            const auto first = flow_test::add(graph, flow_test::sequence());
            const auto second = flow_test::add(graph, flow_test::sequence());
            return std::array{
                flow_test::pin(graph, first, graph::EPinDirection::OUTPUT),
                flow_test::pin(graph, second, graph::EPinDirection::INPUT)
            };
        };
        const auto own = populate(target), other = populate(foreign);
        require(own == other);
        const auto empty_target = sourceBytes(target), empty_foreign = sourceBytes(foreign);
        // Pin payload pointers cannot be passed to connect/disconnect at all (asserted above).
        // Keys are graph-local: equal numbers resolve only in the explicitly selected graph.
        require(target.connect(own[0], own[1]).has_value());
        const auto linked = sourceBytes(target);
        require(sourceBytes(foreign) == empty_foreign);
        require(flow_test::linked(target, own[0]) == std::vector<PinId>{own[1]});
        require(flow_test::linked(target, own[1]) == std::vector<PinId>{own[0]});
        require(flow_test::linked(foreign, own[0]).empty());
        require(flow_test::linked(foreign, own[1]).empty());
        auto moved = std::move(target);
        require(!target.connect(own[0], own[1]));
        require(!target.disconnect(own[0], own[1]));
        require(sourceBytes(moved) == linked);
        require(flow_test::linked(target, own[0]).empty());
        require(flow_test::linked(moved, own[0]) == std::vector<PinId>{own[1]});
        require(moved.disconnect(own[0], own[1]).has_value());
        require(sourceBytes(moved) == empty_target && sourceBytes(foreign) == empty_foreign);
        check(target);
        check(moved);
        check(foreign);
    }

    void checkStoreKeys()
    {
        FlowGraph graph;
        constexpr NodeId high{UINT64_MAX - 1};
        auto detached = flow_test::sequence();
        require(graph.node(high) == nullptr);
        require(graph.addNodeWithId(high, std::move(detached)).has_value());
        const auto* original = graph.node(high);
        require(original != nullptr);
        const auto saved = sourceBytes(graph);
        auto extracted = graph.extractNode(high);
        require(extracted.has_value() && extracted->id == high && extracted->value.definition);
        require(graph.node(high) == nullptr);
        flow_test::restore(graph, *extracted);
        require(sourceBytes(graph) == saved);
        auto replacement = flow_test::sequence();
        replacement.name = "replacement";
        const std::array<NodeId, 1> erased{high};
        const std::array<FlowNodeEntry, 1> inserted{{{high, &replacement}}};
        const auto before = sourceBytes(graph);
        auto edit = FlowGraphEdit::prepare(graph, {.insert = inserted, .erase = erased});
        require(edit.has_value() && sourceBytes(graph) == before && replacement.definition);
        edit->commit();
        auto removed = edit->takeRemoved();
        require(removed.size() == 1 && removed.front().id == high);
        require(removed.front().value.name == extracted->value.name);
        require(graph.node(high)->name == "replacement");
        check(graph);
        const auto last = flow_test::add(graph, flow_test::sequence());
        require(last.value == UINT64_MAX);
        const auto at_limit = sourceBytes(graph);
        auto decoded_limit = decodeFlowSource(at_limit);
        require(decoded_limit.has_value());
        auto rebuilt_limit = materializeFlowSource(*decoded_limit);
        require(rebuilt_limit.has_value() && sourceBytes(*rebuilt_limit) == at_limit);
        require(!graph.addNode(flow_test::sequence()));
        require(graph.removeNode(last).has_value());
        require(!graph.addNode(flow_test::sequence()));
        FlowGraph moved;
        flow_test::add(moved, flow_test::sequence());
        const auto* replacement_pointer = graph.node(high);
        moved = std::move(graph);
        require(moved.node(high) == replacement_pointer && graph.node(high) == nullptr);
        check(moved);
        check(graph);
    }

    void checkPinStore()
    {
        FlowGraph graph;
        constexpr NodeId id{70};
        constexpr PinId high{UINT64_MAX - 1}, last{UINT64_MAX};
        const std::array pins{high, last};
        auto owner = flow_test::sequenceSnapshot(id, 0, pins);
        require(graph.pin(high) == nullptr && graph.pin(last) == nullptr);
        flow_test::restore(graph, owner);
        require(graph.pin(high) && graph.pin(last));
        require(flow_test::pin(graph, id, graph::EPinDirection::INPUT) == high);
        require(flow_test::pin(graph, id, graph::EPinDirection::OUTPUT) == last);
        check(graph);
        const auto encoded = sourceBytes(graph);
        auto decoded = decodeFlowSource(encoded);
        require(decoded.has_value());
        auto rebuilt = materializeFlowSource(*decoded);
        require(rebuilt.has_value() && sourceBytes(*rebuilt) == encoded);
        require(!graph.addNode(flow_test::sequence()));
        const std::array exhausted_pins{high, last, PinId{}};
        auto expanded = flow_test::sequenceSnapshot(id, 1, exhausted_pins);
        const std::array erase{id};
        const std::array<FlowNodeEntry, 1> exhausted_insert{{{id, &expanded.value, expanded.pins}}};
        const auto* original = graph.node(id);
        auto exhausted = FlowGraphEdit::prepare(graph, {.insert = exhausted_insert, .erase = erase});
        require(!exhausted && expanded.value.definition && graph.node(id) == original);
        require(sourceBytes(graph) == encoded);
        check(graph);
        auto extracted = graph.extractNode(id);
        require(extracted.has_value());
        require(flow_test::pinIds(*extracted) == std::vector<PinId>{high, last});
        require(graph.pin(high) == nullptr && graph.pin(last) == nullptr);
        flow_test::restore(graph, *extracted);
        require(sourceBytes(graph) == encoded);
        const auto* input = graph.pin(high);
        const std::array duplicate{high, high};
        auto replacement = flow_test::sequenceSnapshot(id, 0, duplicate);
        const std::array<FlowNodeEntry, 1> insertion{{{id, &replacement.value, replacement.pins}}};
        FlowGraphChange change{.insert = insertion, .erase = erase};
        require(!FlowGraphEdit::prepare(graph, change));
        require(replacement.value.definition && graph.pin(high) == input && sourceBytes(graph) == encoded);
        replacement.pins[1].record.id = last;
        auto prepared = FlowGraphEdit::prepare(graph, change);
        require(prepared.has_value() && graph.pin(high) == input);
        prepared->commit();
        auto removed = prepared->takeRemoved();
        require(removed.size() == 1 && flow_test::pinIds(removed.front()) == std::vector<PinId>{high, last});
        require(graph.pin(high) && graph.pin(last));
        require(sourceBytes(graph) == encoded);
        check(graph);
        const auto* replacement_input = graph.pin(high);
        FlowGraph moved{std::move(graph)};
        require(graph.pin(high) == nullptr && moved.pin(high) == replacement_input);
        check(moved);
        require(moved.removeNode(id).has_value());
        require(moved.pin(high) == nullptr);
        require(!moved.addNode(flow_test::sequence()));
        check(moved);
        FlowGraph dynamic;
        const std::array<PinId, 3> dynamic_pins{{{1}, {2}, last}};
        auto sequence = flow_test::sequenceSnapshot(id, 1, dynamic_pins);
        flow_test::restore(dynamic, sequence);
        const auto dynamic_source = sourceBytes(dynamic);
        const std::array<PinId, 2> retained{{{1}, {2}}};
        auto old = replaceSequence(dynamic, id, 0, retained);
        require(old.pins.back().record.id == last && dynamic.pin(last) == nullptr);
        auto replaced = replaceSequence(dynamic, id, 1, dynamic_pins);
        require(dynamic.pin(last) != nullptr);
        require(flow_test::pin(dynamic, id, graph::EPinDirection::OUTPUT, 1) == last);
        require(sourceBytes(dynamic) == dynamic_source);
        check(dynamic);
    }

    void checkFunctionReferences()
    {
        FlowGraph graph;
        const auto id = flow_test::add(graph, flow_test::function());
        const auto call = flow_test::add(graph, flow_test::call(id, *graph.node(id)));
        const auto returned = flow_test::add(graph, flow_test::returned(id, *graph.node(id)));
        const auto encoded = sourceBytes(graph);
        // The sole candidate gate now also guards extraction; no published missing callee.
        require(!graph.extractNode(id) && sourceBytes(graph) == encoded);
        auto replacement = flow_test::function();
        replacement.name = graph.node(id)->name;
        std::vector<FlowPinEntry> pins;
        for (const auto& record : graph.topology().pins())
        {
            if (record.owner == id)
            {
                auto value = graph.pin(record.id)->clone();
                require(value.has_value());
                pins.push_back({record, std::move(*value)});
            }
        }
        const auto* original = graph.node(id);
        const std::array erase{id};
        const std::array<FlowNodeEntry, 1> insert{{{id, &replacement, pins}}};
        auto edit = FlowGraphEdit::prepare(graph, {.insert = insert, .erase = erase});
        require(edit.has_value() && graph.node(id) == original);
        edit->commit();
        auto old = edit->takeRemoved();
        require(old.size() == 1 && graph.node(id) != original);
        old.clear();
        require(graph.node(call)->payload.get<FunctionCallPayload>()->callee == id);
        require(graph.node(returned)->payload.get<FunctionReturnPayload>()->definition == id);
        require(sourceBytes(graph) == encoded);
        FlowGraph moved(std::move(graph));
        require(sourceBytes(moved) == encoded);
        require(!FlowGraphEdit::prepare(moved, {.erase = erase}));
        require(sourceBytes(moved) == encoded);
        auto wrong_kind = flow_test::sequence();
        const std::array<FlowNodeEntry, 1> kind_insert{{{id, &wrong_kind}}};
        require(!FlowGraphEdit::prepare(moved, {.insert = kind_insert, .erase = erase}));
        const std::vector<FuncArgInfo> typed{{&meta::ref_type_of_v<int>, "value"}};
        auto wrong_signature = flow_test::function(typed, typed);
        const std::array<FlowNodeEntry, 1> signature_insert{{{id, &wrong_signature}}};
        require(!FlowGraphEdit::prepare(moved, {.insert = signature_insert, .erase = erase}));
        require(sourceBytes(moved) == encoded);

        // Preserve the original source defensive checks with explicit test-only fault injection.
        // Normal production writes above cannot publish these invalid reference states.
        auto* call_payload = const_cast<FlowNode*>(moved.node(call))->payload.get<FunctionCallPayload>();
        auto* return_payload = const_cast<FlowNode*>(moved.node(returned))->payload.get<FunctionReturnPayload>();
        for (const auto invalid : {NodeId{9999}, call})
        {
            call_payload->callee = invalid;
            return_payload->definition = invalid;
            require(!captureFlowNode(moved, call));
            require(!captureFlowNode(moved, returned));
        }
        call_payload->callee = id;
        return_payload->definition = id;
        call_payload->arguments = typed;
        return_payload->results = typed;
        require(!captureFlowNode(moved, call));
        require(!captureFlowNode(moved, returned));
        call_payload->arguments.clear();
        return_payload->results.clear();
        require(sourceBytes(moved) == encoded);
        std::puts("Flow references: atomic rebuild/move, missing/kind/signature refusal and source defense");
    }

    void checkMaximumCallee()
    {
        FlowGraph graph;
        require(graph.addNodeWithId({UINT64_MAX}, flow_test::function()).has_value());
        const auto* definition = graph.node({UINT64_MAX});
        require(graph.addNodeWithId({1}, flow_test::call({UINT64_MAX}, *definition)).has_value());
        require(graph.addNodeWithId({2}, flow_test::returned({UINT64_MAX}, *definition)).has_value());
        const auto encoded = sourceBytes(graph);
        auto decoded = decodeFlowSource(encoded);
        require(decoded.has_value());
        auto rebuilt = materializeFlowSource(*decoded);
        require(rebuilt.has_value() && sourceBytes(*rebuilt) == encoded);
        FlowSourceVariable variable;
        variable.id = UINT64_MAX;
        require(!validateFlowVariable(variable));
    }

    void checkFunctionReferenceBatch()
    {
        FlowGraph graph;
        constexpr NodeId id{41};
        auto definition = flow_test::function();
        auto call = flow_test::call(id, definition);
        auto returned = flow_test::returned(id, definition);
        const std::array<FlowNodeEntry, 3> insert{{{{42}, &call}, {{43}, &returned}, {id, &definition}}};
        auto plan = FlowGraphEdit::prepare(graph, {.insert = insert});
        require(plan.has_value() && definition.definition && call.definition && returned.definition);
        require(graph.nodes().empty());
        plan->commit();
        require(graph.node({42})->payload.get<FunctionCallPayload>()->callee == id);
        const auto encoded = sourceBytes(graph);
        auto incompatible = flow_test::function({{&meta::ref_type_of_v<int>, "value"}});
        auto invalid = flow_test::call(id, incompatible);
        const std::array<FlowNodeEntry, 1> invalid_insert{{{{44}, &invalid}}};
        auto refused = FlowGraphEdit::prepare(graph, {.insert = invalid_insert});
        require(!refused && invalid.definition && sourceBytes(graph) == encoded);
        const std::array<NodeId, 3> erase{{{41}, {42}, {43}}};
        auto remove = FlowGraphEdit::prepare(graph, {.erase = erase});
        require(remove.has_value());
        remove->commit();
        auto removed = remove->takeRemoved();
        require(graph.nodes().empty() && removed.size() == 3);
    }
} // namespace

int main()
{
    checkReceiver();
    checkStoreKeys();
    checkPinStore();
    checkFunctionReferences();
    checkMaximumCallee();
    checkFunctionReferenceBatch();
    FlowGraph graph;
    const auto id = flow_test::add(graph, flow_test::sequence(1));
    require(graph.node(id) != nullptr);
    flow_test::place(graph, id, {9.0F, 17.0F, true});
    check(graph);
    const auto target = flow_test::add(graph, flow_test::sequence());
    const auto target_input = flow_test::pin(graph, target, graph::EPinDirection::INPUT);
    const auto pin_id = flow_test::pin(graph, id, graph::EPinDirection::OUTPUT, 1);
    require(pin_id.valid());
    require(graph.connect(pin_id, target_input).has_value());
    check(graph);
    const auto linked = sourceBytes(graph);
    const std::array retained{
        flow_test::pin(graph, id, graph::EPinDirection::INPUT),
        flow_test::pin(graph, id, graph::EPinDirection::OUTPUT)
    };
    auto previous = replaceSequence(graph, id, 0, retained);
    require(previous.pins.back().record.id == pin_id);
    require(graph.pin(pin_id) == nullptr && graph.topology().findPin(pin_id) == nullptr);
    require(graph.topology().links().empty());
    check(graph);
    const std::array restored_pins{retained[0], retained[1], pin_id};
    const std::array<graph::LinkRecord, 1> restored_links{{{pin_id, target_input}}};
    auto replaced = replaceSequence(graph, id, 1, restored_pins, restored_links);
    require(flow_test::pin(graph, id, graph::EPinDirection::OUTPUT, 1) == pin_id);
    require(sourceBytes(graph) == linked);
    check(graph);
    const auto decoded = decodeFlowSource(linked);
    require(decoded.has_value());
    auto rebuilt = materializeFlowSource(*decoded);
    require(rebuilt.has_value());
    check(*rebuilt);
    require(sourceBytes(*rebuilt) == linked);
    const auto* sequence = graph.node(id);
    FlowGraph moved{std::move(graph)};
    check(moved);
    require(sourceBytes(moved) == linked && moved.node(id) == sequence && graph.node(id) == nullptr);
    graph = std::move(moved);
    check(graph);
    require(graph.node(id) == sequence && moved.node(id) == nullptr);
    const std::array<graph::GraphLayoutEntry, 1> saved_layout{{{id, *graph.layout().find(id)}}};
    const std::array<NodeId, 1> erased{id};
    auto edit = FlowGraphEdit::prepare(graph, {.erase = erased});
    require(edit.has_value());
    require(sourceBytes(graph) == linked);
    edit->commit();
    auto removed = edit->takeRemoved();
    require(removed.size() == 1 && removed.front().id == id);
    require(removed.front().value.payload.get<SequencePayload>()->additional_outputs == 1);
    require(graph.node(id) == nullptr && graph.pin(pin_id) == nullptr && graph.topology().links().empty());
    check(graph);
    const std::array<FlowNodeEntry, 1> insert{{{removed.front().id, &removed.front().value, removed.front().pins}}};
    auto undo = FlowGraphEdit::prepare(graph, {.insert = insert, .connect = restored_links, .place = saved_layout});
    require(undo.has_value());
    undo->commit();
    removed.clear();
    require(graph.node(id) != nullptr);
    check(graph);
    require(sourceBytes(graph) == linked);
    std::puts("PASS: controlled structure, dynamic pins, source roundtrip, graph move and edit replay");
}
