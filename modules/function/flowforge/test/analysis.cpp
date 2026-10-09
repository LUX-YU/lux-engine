#if defined(FLOW_ANALYSIS_COMPILER)
#include <lux/engine/flowforge/Compiler.hpp>
#else
#include <lux/engine/flowforge/FlowAnalysis.hpp>
#endif
#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityPayload.hpp>
#include <lux/engine/flowforge/script/ScriptEventPayload.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool value, std::source_location at = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "Flow analysis failed at %u\n", at.line());
            std::abort();
        }
    }

#if defined(FLOW_ANALYSIS_COMPILER)
    using Options = FlowForgeCompileOptions;

    auto analyze(const FlowGraph& graph, Options options = {}) noexcept
    {
        options.module_name = "analysis_contract";
        return compileFlowForgeObject(graph, options);
    }
#else
    using Options = FlowAnalysisOptions;

    auto analyze(const FlowGraph& graph, const Options& options = {}) noexcept
    {
        return FlowAnalysis::create(graph, options);
    }
#endif

    std::shared_ptr<const FlowNodeType> definition(std::string_view name) noexcept
    {
        FlowNodeCatalog catalog;
        require(catalog.add(functionNodeRegistrations()).has_value());
        require(catalog.add(controlNodeRegistrations()).has_value());
        const std::array script{scriptAbilityRegistration(), scriptEventRegistration()};
        require(catalog.add(script).has_value());
        auto result = catalog.find(graph::nodeTypeId(name));
        require(result != nullptr);
        return result;
    }

    template <class T> NodeId add(FlowGraph& graph, std::string_view type, T value, std::string name = {})
    {
        auto registered = definition(type);
        auto payload = registered->create();
        require(payload.has_value());
        *payload->get<T>() = std::move(value);
        auto node = createFlowNode(std::move(registered), std::move(*payload));
        require(node.has_value());
        node->name = std::move(name);
        auto inserted = graph.addNode(std::move(*node));
        require(inserted.has_value());
        return *inserted;
    }

    NodeId entry(FlowGraph& graph)
    {
        const auto result = add(graph, "lux.flow.event", EventEntryPayload{}, "Tick");
        require(graph.addExport({{1}, result, 41, {}}));
        return result;
    }

    PinId pin(
        const FlowGraph& graph,
        NodeId owner,
        graph::EPinDirection direction,
        EFlowPinRole role,
        std::size_t index = 0
    ) noexcept
    {
        for (const auto& record : graph.topology().pins())
        {
            const bool matches =
                record.owner == owner && record.direction == direction && graph.pin(record.id)->role == role;
            if (matches)
            {
                if (index == 0)
                {
                    return record.id;
                }
                --index;
            }
        }
        require(false);
        return {};
    }

    PinId input(const FlowGraph& graph, NodeId node) noexcept
    {
        return pin(graph, node, graph::EPinDirection::INPUT, EFlowPinRole::EXECUTION);
    }

    PinId output(const FlowGraph& graph, NodeId node, std::size_t index = 0) noexcept
    {
        return pin(graph, node, graph::EPinDirection::OUTPUT, EFlowPinRole::EXECUTION, index);
    }

    void link(FlowGraph& graph, PinId from, PinId to)
    {
        require(graph.connect(from, to).has_value());
    }

    template <class Result> void failure(const Result& result, EFlowForgeError code, NodeId node, PinId pin = {})
    {
        require(!result);
        require(result.error().code == code);
        require(result.error().node_id == node.value && result.error().pin_id == pin.value);
        std::printf(
            "%u %llu %llu %s\n",
            static_cast<unsigned>(code),
            static_cast<unsigned long long>(node.value),
            static_cast<unsigned long long>(pin.value),
            result.error().message.c_str()
        );
    }

    ScriptAbilityNodeDescription description()
    {
        return {
            .contract = script::ScriptApiContractIdView{"test.analysis"},
            .method = script::ScriptApiMethodIdView{"query"},
            .schema_hash = 73
        };
    }

    void requirements()
    {
        FlowGraph graph;
        const auto start = entry(graph);
        auto desc = description();
        const auto ability = add(graph, "lux.flow.ability_call", ScriptAbilityPayload{desc});
        link(graph, output(graph, start), input(graph, ability));
        failure(analyze(graph), EFlowForgeError::UNKNOWN_SCRIPT_ABILITY_CONTRACT, ability);
        auto catalog = desc;
        catalog.method = script::ScriptApiMethodIdView{"other"};
        Options options{.script_abilities = ScriptAbilityNodeCatalogView{{&catalog, 1}}};
        failure(analyze(graph, options), EFlowForgeError::UNKNOWN_SCRIPT_ABILITY_METHOD, ability);
        catalog = desc;
        ++catalog.schema_hash;
        failure(analyze(graph, options), EFlowForgeError::SCRIPT_ABILITY_SCHEMA_MISMATCH, ability);
        catalog = desc;
        auto success = analyze(graph, options);
        require(success.has_value());
#if !defined(FLOW_ANALYSIS_COMPILER)
        require(success->abilityRequirements().size() == 1);
        require(success->abilityRequirements()[0].contract.name() == desc.contract.name());
        require(success->abilityRequirements()[0].expected_schema_hash == 73);
        require(!success->firstSuspensionFrom(output(graph, start)).valid());
#endif
        ++desc.schema_hash;
        const auto conflicting = add(graph, "lux.flow.ability_call", ScriptAbilityPayload{desc});
        failure(analyze(graph, options), EFlowForgeError::SCRIPT_ABILITY_REQUIREMENT_CONFLICT, conflicting);

        FlowGraph impostor;
        entry(impostor);
        const auto fake = add(impostor, "lux.flow.ability_call", ScriptAbilityPayload{desc});
        // Deliberately corrupt an already admitted semantic payload. Normal catalog admission
        // cannot register this impostor; analysis must still reject malformed input defensively.
        auto malformed = definition("lux.flow.function")->create();
        require(malformed.has_value());
        // Test-only fault injection into a non-const graph; no mutable-node SDK escape is added.
        const_cast<FlowNode*>(impostor.node(fake))->payload = std::move(*malformed);
        failure(analyze(impostor), EFlowForgeError::GRAPH_INVALID, fake);
    }

    script::ScriptEventSourceDescription eventDescription()
    {
        return {
            .system_name = "test.simulation",
            .event_name = "arrived",
            .system_id = 7,
            .event_id = 9,
            .payload =
                {"lux.i32", semantic::typeId("lux.i32"), static_cast<std::uint8_t>(semantic::EAbiKind::I32), 4, 4},
            .payload_schema_hash = 11,
            .payload_schema_version = 1,
            .delivery_hook_id = 13,
            .delivery_schema_hash = 15,
            .delivery_schema_version = 1
        };
    }

    void events()
    {
        FlowGraph graph;
        const auto start = entry(graph);
        auto desc = eventDescription();
        require(desc.valid());
        const auto event = add(graph, "lux.flow.event_wait", ScriptEventPayload{desc});
        link(graph, output(graph, start), input(graph, event));
        failure(analyze(graph), EFlowForgeError::UNKNOWN_SCRIPT_EVENT_SOURCE, event);
        auto catalog = desc;
        ++catalog.delivery_schema_hash;
        Options options{.script_events = {&catalog, 1}};
        failure(analyze(graph, options), EFlowForgeError::SCRIPT_EVENT_SCHEMA_MISMATCH, event);
        catalog = desc;
        options.lifecycle.begin_play = 41;
        failure(analyze(graph, options), EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED, event);
#if !defined(FLOW_ANALYSIS_COMPILER)
        options.lifecycle = {};
        auto result = analyze(graph, options);
        require(result.has_value() && result->eventRequirements().size() == 1);
        require(result->eventRequirements()[0] == desc);
        require(result->firstSuspensionFrom(output(graph, start)) == event);
        require(result->suspensionBetween(output(graph, start), event) == event);
        catalog.system_name = "changed after analysis";
        require(result->eventRequirements()[0] == desc);
#endif
    }

    void transitive()
    {
        FlowGraph graph;
        const auto start = entry(graph);
        const auto first = add(graph, "lux.flow.function", FunctionPayload{}, "First");
        const auto second = add(graph, "lux.flow.function", FunctionPayload{}, "Second");
        const auto call = add(graph, "lux.flow.function_call", FunctionCallPayload{first});
        const auto nested = add(graph, "lux.flow.function_call", FunctionCallPayload{second});
        const auto recurse = add(graph, "lux.flow.function_call", FunctionCallPayload{first});
        const auto sequence = add(graph, "lux.flow.sequence", SequencePayload{1});
        auto desc = description();
        desc.kind = script::EScriptApiMethodKind::ASYNC_OPERATION;
        const auto asynchronous = add(graph, "lux.flow.ability_call", ScriptAbilityPayload{desc});
        const auto later = add(graph, "lux.flow.ability_call", ScriptAbilityPayload{desc});
        link(graph, output(graph, start), input(graph, call));
        link(graph, output(graph, first), input(graph, nested));
        link(graph, output(graph, second), input(graph, sequence));
        link(graph, output(graph, sequence), input(graph, later));
        link(graph, output(graph, sequence, 1), input(graph, asynchronous));
        link(graph, output(graph, later), input(graph, recurse));
        Options options{.script_abilities = ScriptAbilityNodeCatalogView{{&desc, 1}}};
        options.lifecycle.begin_play = 41;
        failure(analyze(graph, options), EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED, asynchronous);
        options.lifecycle.begin_play = 0;
        options.lifecycle.end_play = 41;
        failure(analyze(graph, options), EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED, asynchronous);
#if !defined(FLOW_ANALYSIS_COMPILER)
        options.lifecycle = {};
        auto result = analyze(graph, options);
        require(result.has_value());
        require(result->firstSuspensionFrom(output(graph, start)) == asynchronous);
        require(result->suspensionBetween(output(graph, start), call) == asynchronous);
        require(!result->firstSuspensionFrom({}).valid());
        require(!result->suspensionBetween(output(graph, start), {UINT64_MAX}).valid());
        require(!result->firstSuspensionFrom(input(graph, call)).valid());
#endif
    }

    void foreignCallee()
    {
        FlowGraph foreign;
        const auto foreign_definition = add(foreign, "lux.flow.function", FunctionPayload{}, "Foreign");
        FlowGraph graph;
        const auto start = entry(graph);
        const auto function = add(graph, "lux.flow.function", FunctionPayload{}, "Local");
        // The real graph admission now rejects a foreign target before analysis.
        auto registered = definition("lux.flow.function_call");
        auto payload = registered->create();
        require(payload.has_value());
        payload->get<FunctionCallPayload>()->callee = foreign_definition;
        auto candidate = createFlowNode(registered, std::move(*payload));
        require(candidate.has_value());
        require(!graph.addNode(std::move(*candidate)));
        const auto call = add(graph, "lux.flow.function_call", FunctionCallPayload{function});
        // Keep the original analysis defense and diagnostic assertion too.
        link(graph, output(graph, function), input(graph, call));
        const_cast<FlowNode*>(graph.node(call))->payload.get<FunctionCallPayload>()->callee = foreign_definition;
        require(foreign_definition == start);
        failure(analyze(graph), EFlowForgeError::GRAPH_INVALID, function);
    }

    void rebuiltCallee()
    {
        FlowGraph graph;
        const auto start = entry(graph);
        const auto function = add(graph, "lux.flow.function", FunctionPayload{}, "Rebuilt");
        const auto call = add(graph, "lux.flow.function_call", FunctionCallPayload{function});
        const auto returned = add(graph, "lux.flow.function_return", FunctionReturnPayload{function});
        link(graph, output(graph, start), input(graph, call));
        link(graph, output(graph, function), input(graph, returned));
        require(analyze(graph).has_value());
        const auto* original = graph.node(function);
        // Rebuild a referenced definition atomically, never exposing a missing callee.
        auto registered = definition("lux.flow.function");
        auto payload = registered->create();
        require(payload.has_value());
        auto replacement = createFlowNode(registered, std::move(*payload));
        require(replacement.has_value());
        replacement->name = "Rebuilt";
        const FlowNodeEntry entry{function, &*replacement, {}};
        auto prepared = FlowGraphEdit::prepare(graph, {.insert = {&entry, 1}, .erase = {&function, 1}});
        require(prepared.has_value());
        prepared->commit();
        auto old = prepared->takeRemoved();
        require(old.size() == 1 && old.front().id == function);
        require(graph.node(function) != original);
        old.clear();
        link(graph, output(graph, function), input(graph, returned));
        require(analyze(graph).has_value());
    }

    void borrowed()
    {
        FlowGraph graph;
        const auto start = entry(graph);
        const auto value = script::ScriptAbilityValueDescription{
            semantic::typeId("lux.i32"),
            "lux.i32",
            semantic::EValuePass::VALUE,
            static_cast<std::uint8_t>(semantic::EAbiKind::I32),
            4,
            4,
            script::EScriptAbilityValueLifetime::BORROWED_STEP
        };
        const std::array outputs{value};
        const std::array inputs{script::ScriptAbilityParameterDescription{"input", value}};
        auto query = description();
        query.results = outputs;
        auto asynchronous = description();
        asynchronous.method = script::ScriptApiMethodIdView{"async"};
        asynchronous.kind = script::EScriptApiMethodKind::ASYNC_OPERATION;
        auto consume = description();
        consume.method = script::ScriptApiMethodIdView{"consume"};
        consume.parameters = inputs;
        const std::array catalog{query, asynchronous, consume};
        const auto producer = add(graph, "lux.flow.ability_call", ScriptAbilityPayload{query});
        const auto suspend = add(graph, "lux.flow.ability_call", ScriptAbilityPayload{asynchronous});
        const auto consumer = add(graph, "lux.flow.ability_call", ScriptAbilityPayload{consume});
        link(graph, output(graph, start), input(graph, producer));
        link(graph, output(graph, producer), input(graph, suspend));
        link(graph, output(graph, suspend), input(graph, consumer));
        require(graph
                    .connect(
                        pin(graph, producer, graph::EPinDirection::OUTPUT, EFlowPinRole::DATA),
                        pin(graph, consumer, graph::EPinDirection::INPUT, EFlowPinRole::DATA)
                    )
                    .has_value());
        Options options{.script_abilities = ScriptAbilityNodeCatalogView{catalog}};
        failure(
            analyze(graph, options),
            EFlowForgeError::BORROWED_VALUE_CROSSES_SUSPENSION,
            suspend,
            pin(graph, producer, graph::EPinDirection::OUTPUT, EFlowPinRole::DATA)
        );
#if !defined(FLOW_ANALYSIS_COMPILER)
        require(graph.disconnect(output(graph, suspend), input(graph, consumer)).has_value());
        require(graph.disconnect(output(graph, producer), input(graph, suspend)).has_value());
        link(graph, output(graph, producer), input(graph, consumer));
        auto valid = analyze(graph, options);
        require(valid.has_value());
        require(!valid->firstSuspensionFrom(output(graph, start)).valid());
#endif
    }

#if !defined(FLOW_ANALYSIS_COMPILER)
    FlowAnalysis frozen(graph::PinId& start_id, graph::NodeId& witness)
    {
        FlowGraph graph;
        const auto start = entry(graph);
        auto desc = description();
        desc.kind = script::EScriptApiMethodKind::ASYNC_OPERATION;
        const auto node = add(graph, "lux.flow.ability_call", ScriptAbilityPayload{desc});
        link(graph, output(graph, start), input(graph, node));
        start_id = output(graph, start);
        witness = node;
        auto result = analyze(graph, {.script_abilities = ScriptAbilityNodeCatalogView{{&desc, 1}}});
        require(result.has_value());
        return std::move(*result);
    }
#endif
} // namespace

int main()
{
    requirements();
    events();
    transitive();
    borrowed();
    foreignCallee();
    rebuiltCallee();
#if !defined(FLOW_ANALYSIS_COMPILER)
    graph::PinId start;
    graph::NodeId witness;
    auto result = frozen(start, witness);
    require(result.firstSuspensionFrom(start) == witness);
    require(result.suspensionBetween(start, witness) == witness);
    require(result.abilityRequirements()[0].contract.name() == "test.analysis");
#endif
}
