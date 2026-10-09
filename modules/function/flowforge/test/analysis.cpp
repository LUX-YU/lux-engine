#if defined(FLOW_ANALYSIS_COMPILER)
#include <lux/engine/flowforge/Compiler.hpp>
#else
#include <lux/engine/flowforge/FlowAnalysis.hpp>
#endif
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>

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

    template <class T, class... Args> T& add(FlowGraph& graph, Args&&... args)
    {
        auto node = std::make_unique<T>(std::forward<Args>(args)...);
        auto& result = *node;
        require(graph.addNodes(std::move(node)) != 0);
        return result;
    }

    OnEventNode& entry(FlowGraph& graph)
    {
        auto& result = add<OnEventNode>(graph, "Tick");
        require(graph.addExport({{1}, result.id(), 41, {}}));
        return result;
    }

    void link(ExecOutPin& from, ExecInPin& to)
    {
        require(from.linkTo(&to) == ELinkError::SUCCESS);
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
        auto& start = entry(graph);
        auto desc = description();
        auto& ability = add<ScriptAbilityNode>(graph, desc);
        link(start.execOutPin(), ability.execInPin());
        failure(analyze(graph), EFlowForgeError::UNKNOWN_SCRIPT_ABILITY_CONTRACT, ability.id());
        auto catalog = desc;
        catalog.method = script::ScriptApiMethodIdView{"other"};
        Options options{.script_abilities = ScriptAbilityNodeCatalogView{{&catalog, 1}}};
        failure(analyze(graph, options), EFlowForgeError::UNKNOWN_SCRIPT_ABILITY_METHOD, ability.id());
        catalog = desc;
        ++catalog.schema_hash;
        failure(analyze(graph, options), EFlowForgeError::SCRIPT_ABILITY_SCHEMA_MISMATCH, ability.id());
        catalog = desc;
        auto success = analyze(graph, options);
        require(success.has_value());
#if !defined(FLOW_ANALYSIS_COMPILER)
        require(success->abilityRequirements().size() == 1);
        require(success->abilityRequirements()[0].contract.name() == desc.contract.name());
        require(success->abilityRequirements()[0].expected_schema_hash == 73);
        require(!success->firstSuspensionFrom(start.execOutPin().id()).valid());
#endif
        ++desc.schema_hash;
        auto& conflicting = add<ScriptAbilityNode>(graph, desc);
        failure(analyze(graph, options), EFlowForgeError::SCRIPT_ABILITY_REQUIREMENT_CONFLICT, conflicting.id());

        FlowGraph impostor;
        entry(impostor);
        auto& fake = add<Node>(impostor, 0, ENodeOperation::SCRIPT_ABILITY_CALL);
        failure(analyze(impostor), EFlowForgeError::GRAPH_INVALID, fake.id());
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
        auto& start = entry(graph);
        auto desc = eventDescription();
        require(desc.valid());
        auto& event = add<ScriptEventAwaitNode>(graph, desc);
        link(start.execOutPin(), event.execInPin());
        failure(analyze(graph), EFlowForgeError::UNKNOWN_SCRIPT_EVENT_SOURCE, event.id());
        auto catalog = desc;
        ++catalog.delivery_schema_hash;
        Options options{.script_events = {&catalog, 1}};
        failure(analyze(graph, options), EFlowForgeError::SCRIPT_EVENT_SCHEMA_MISMATCH, event.id());
        catalog = desc;
        options.lifecycle.begin_play = 41;
        failure(analyze(graph, options), EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED, event.id());
#if !defined(FLOW_ANALYSIS_COMPILER)
        options.lifecycle = {};
        auto result = analyze(graph, options);
        require(result.has_value() && result->eventRequirements().size() == 1);
        require(result->eventRequirements()[0] == desc);
        require(result->firstSuspensionFrom(start.execOutPin().id()) == event.id());
        require(result->suspensionBetween(start.execOutPin().id(), event.id()) == event.id());
        catalog.system_name = "changed after analysis";
        require(result->eventRequirements()[0] == desc);
#endif
    }

    void transitive()
    {
        FlowGraph graph;
        auto& start = entry(graph);
        auto& first = add<FuncDefNode>(graph, "First", std::vector<FuncArgInfo>{});
        auto& second = add<FuncDefNode>(graph, "Second", std::vector<FuncArgInfo>{});
        auto& call = add<GraphFuncCallNode>(graph, first);
        auto& nested = add<GraphFuncCallNode>(graph, second);
        auto& recurse = add<GraphFuncCallNode>(graph, first);
        auto& sequence = add<SequenceNode>(graph);
        auto desc = description();
        desc.kind = script::EScriptApiMethodKind::ASYNC_OPERATION;
        auto& asynchronous = add<ScriptAbilityNode>(graph, desc);
        auto& later = add<ScriptAbilityNode>(graph, desc);
        link(start.execOutPin(), call.execInPin());
        link(first.execOutPin(), nested.execInPin());
        link(second.execOutPin(), sequence.execInPin());
        link(sequence.execOutPin(), later.execInPin());
        link(*sequence.addExecOutPin(), asynchronous.execInPin());
        link(later.execOutPin(), recurse.execInPin());
        Options options{.script_abilities = ScriptAbilityNodeCatalogView{{&desc, 1}}};
        options.lifecycle.begin_play = 41;
        failure(analyze(graph, options), EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED, asynchronous.id());
        options.lifecycle.begin_play = 0;
        options.lifecycle.end_play = 41;
        failure(analyze(graph, options), EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED, asynchronous.id());
#if !defined(FLOW_ANALYSIS_COMPILER)
        options.lifecycle = {};
        auto result = analyze(graph, options);
        require(result.has_value());
        require(result->firstSuspensionFrom(start.execOutPin().id()) == asynchronous.id());
        require(result->suspensionBetween(start.execOutPin().id(), call.id()) == asynchronous.id());
        require(!result->firstSuspensionFrom({}).valid());
        require(!result->suspensionBetween(start.execOutPin().id(), {UINT64_MAX}).valid());
        require(!result->firstSuspensionFrom(call.execInPin().id()).valid());
#endif
    }

    void foreignCallee()
    {
        FlowGraph foreign;
        auto& foreign_definition = add<FuncDefNode>(foreign, "Foreign", std::vector<FuncArgInfo>{});
        FlowGraph graph;
        auto& start = entry(graph);
        auto& function = add<FuncDefNode>(graph, "Local", std::vector<FuncArgInfo>{});
        auto& call = add<GraphFuncCallNode>(graph, foreign_definition);
        link(function.execOutPin(), call.execInPin());
        require(foreign_definition.id() == start.id());
        failure(analyze(graph), EFlowForgeError::GRAPH_INVALID, function.id());
    }

    void borrowed()
    {
        FlowGraph graph;
        auto& start = entry(graph);
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
        auto& producer = add<ScriptAbilityNode>(graph, query);
        auto& suspend = add<ScriptAbilityNode>(graph, asynchronous);
        auto& consumer = add<ScriptAbilityNode>(graph, consume);
        link(start.execOutPin(), producer.execInPin());
        link(producer.execOutPin(), suspend.execInPin());
        link(suspend.execOutPin(), consumer.execInPin());
        require(producer.resultPins()[0]->linkTo(consumer.parameterPins()[0].get()) == ELinkError::SUCCESS);
        Options options{.script_abilities = ScriptAbilityNodeCatalogView{catalog}};
        failure(
            analyze(graph, options),
            EFlowForgeError::BORROWED_VALUE_CROSSES_SUSPENSION,
            suspend.id(),
            producer.resultPins()[0]->id()
        );
#if !defined(FLOW_ANALYSIS_COMPILER)
        require(suspend.execOutPin().unlinkFrom(&consumer.execInPin()) == ELinkError::UNLINKED);
        require(producer.execOutPin().unlinkFrom(&suspend.execInPin()) == ELinkError::UNLINKED);
        link(producer.execOutPin(), consumer.execInPin());
        auto valid = analyze(graph, options);
        require(valid.has_value());
        require(!valid->firstSuspensionFrom(start.execOutPin().id()).valid());
#endif
    }

#if !defined(FLOW_ANALYSIS_COMPILER)
    FlowAnalysis frozen(graph::PinId& start_id, graph::NodeId& witness)
    {
        FlowGraph graph;
        auto& start = entry(graph);
        auto desc = description();
        desc.kind = script::EScriptApiMethodKind::ASYNC_OPERATION;
        auto& node = add<ScriptAbilityNode>(graph, desc);
        link(start.execOutPin(), node.execInPin());
        start_id = start.execOutPin().id();
        witness = node.id();
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
#if !defined(FLOW_ANALYSIS_COMPILER)
    graph::PinId start;
    graph::NodeId witness;
    auto result = frozen(start, witness);
    require(result.firstSuspensionFrom(start) == witness);
    require(result.suspensionBetween(start, witness) == witness);
    require(result.abilityRequirements()[0].contract.name() == "test.analysis");
#endif
}
