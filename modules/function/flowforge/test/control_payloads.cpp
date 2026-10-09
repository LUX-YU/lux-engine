#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FlowExecutionCompiler.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/ObjectNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/NodeRegistry.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityPayload.hpp>
#include <lux/engine/flowforge/script/ScriptEventPayload.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool condition, std::source_location at = std::source_location::current()) noexcept
    {
        if (!condition)
        {
            std::fprintf(stderr, "control payload contract failed at %u\n", at.line());
            std::exit(42);
        }
    }

    template <class T> FlowForgeResult<std::unique_ptr<T>> clone(const T& value) noexcept
    {
        return std::make_unique<T>(value);
    }

    // Real payload/schema and GraphEdit tests, not a control compiler qualification.
    template <class T> FlowNodeRegistration registration(std::string name) noexcept
    {
        FlowNodeRegistration result;
        result.identity = {graph::nodeTypeId(name), std::move(name), 1};
        result.payload_type = cxx::typeToken<T>();
        result.create = [](const object::CodeLease& code) noexcept { return FlowNodePayload::make<T, clone<T>>(code); };
        result.describe_pins = [](const FlowNodePayload& value) noexcept { return value.get<T>()->describePins(); };
        result.validate = [](const FlowNodePayload&) noexcept -> FlowForgeResult<void> { return {}; };
        result.compile = [](const FlowNodePayload&,
                            std::span<const FlowValue>,
                            FlowValueCompiler&) noexcept -> FlowNodeRegistration::ValueResult
        { return cxx::unexpected(FlowForgeFailure{EFlowForgeError::LOWERING_FAILED, "schema test has no compiler"}); };
        if constexpr (requires(const T& value, FlowReferenceView view) { value.validateReferences(view); })
        {
            result.validate_references = [](const FlowNodePayload& payload, FlowReferenceView view) noexcept
            { return payload.get<T>()->validateReferences(view); };
        }
        return result;
    }

    FlowNode makeNode(const FlowNodeCatalog& catalog, std::string_view name) noexcept
    {
        auto definition = catalog.find(graph::nodeTypeId(name));
        require(definition != nullptr);
        auto payload = definition->create();
        require(payload.has_value());
        auto result = createFlowNode(std::move(definition), std::move(*payload));
        require(result.has_value());
        return std::move(*result);
    }

    PinId pinNamed(const FlowGraph& graph, NodeId node, std::string_view name) noexcept
    {
        for (const auto& record : graph.topology().pins())
        {
            if (record.owner == node && graph.pin(record.id)->name == name)
            {
                return record.id;
            }
        }
        require(false);
        return {};
    }

    int initializer_calls{};

    template <class T> FlowNode makePayloadNode(const FlowNodeCatalog& catalog, std::string_view name, T value) noexcept
    {
        auto payload = FlowNodePayload::make<T, clone<T>>(object::CodeLease::builtin(), std::move(value));
        require(payload.has_value());
        auto result = createFlowNode(catalog.find(graph::nodeTypeId(name)), std::move(*payload));
        require(result.has_value());
        return std::move(*result);
    }

    struct InitialPayload final
    {
        bool reject{};
        bool wrong_type{};
        graph::EPinDirection direction{graph::EPinDirection::INPUT};
        EFlowPinRole role{EFlowPinRole::DATA};
        bool allow_default{true};

        FlowNodeRegistration::PinResult describePins() const noexcept
        {
            FlowPinDeclaration pin{
                graph::PinSemanticId{1},
                "value",
                graph::EPinDirection::INPUT,
                &meta::ref_type_of_v<std::int32_t>,
                true
            };
            pin.direction = direction;
            pin.role = role;
            pin.allow_default = allow_default;
            if (role == EFlowPinRole::EXECUTION)
            {
                pin.type = nullptr;
            }
            pin.initial_value = [](const FlowNodePayload& erased,
                                   const meta::RefType&) noexcept -> FlowForgeResult<meta::RuntimeObject>
            {
                ++initializer_calls;
                const auto& value = *erased.get<InitialPayload>();
                if (value.reject)
                {
                    return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "initializer refused"});
                }
                if (value.wrong_type)
                {
                    return meta::RuntimeObject{true};
                }
                return meta::RuntimeObject{std::int32_t{41}};
            };
            return std::vector{pin};
        }
    };

    void preparation(FlowNodeCatalog& catalog) noexcept
    {
        const auto registered = registration<InitialPayload>("tests.initializer");
        require(catalog.add({&registered, 1}).has_value());
        FlowGraph graph;
        auto node = makeNode(catalog, "tests.initializer");
        require(initializer_calls == 0); // Describing a schema never creates values.
        const auto invalid_schema = [&](InitialPayload value) noexcept
        {
            auto payload =
                FlowNodePayload::make<InitialPayload, clone<InitialPayload>>(object::CodeLease::builtin(), value);
            require(payload.has_value());
            const auto refused = createFlowNode(node.definition, std::move(*payload));
            require(!refused && initializer_calls == 0);
        };
        auto invalid = InitialPayload{};
        invalid.direction = graph::EPinDirection::OUTPUT;
        invalid_schema(invalid);
        invalid = {};
        invalid.allow_default = false;
        invalid.role = EFlowPinRole::EXECUTION;
        invalid_schema(invalid);
        const FlowNodeEntry entry{NodeId{21}, &node, {}};
        FlowGraphChange change;
        change.insert = {&entry, 1};
        auto prepared = FlowGraphEdit::prepare(graph, change);
        require(prepared.has_value() && initializer_calls == 1 && graph.topology().nodes().empty());
        prepared->commit();
        require(initializer_calls == 1); // Commit has no arbitrary callback.
        const auto id = pinNamed(graph, entry.id, "value");
        require(graph.pin(id)->default_value.get<std::int32_t>() == 41);
        require(graph.pin(id)->setDefault(meta::RuntimeObject{std::int32_t{97}}));
        auto saved = graph.extractNode(entry.id);
        require(saved.has_value());
        saved->value.payload.get<InitialPayload>()->reject = true;
        const FlowNodeEntry restore{saved->id, &saved->value, saved->pins};
        change.insert = {&restore, 1};
        auto restored = FlowGraphEdit::prepare(graph, change);
        require(restored.has_value() && initializer_calls == 1);
        restored->commit();
        require(graph.pin(id)->default_value.get<std::int32_t>() == 97);
        const auto* original = graph.node(entry.id);
        const auto* original_pin = graph.pin(id);
        const graph::GraphLayoutEntry placement{entry.id, {7, 13, true}};
        FlowGraphChange placed;
        placed.place = {&placement, 1};
        auto layout = FlowGraphEdit::prepare(graph, placed);
        require(layout.has_value());
        layout->commit();
        node.payload.get<InitialPayload>()->reject = true;
        change.insert = {&entry, 1};
        change.erase = {&entry.id, 1};
        auto refused = FlowGraphEdit::prepare(graph, change);
        require(!refused && initializer_calls == 2);
        auto* failure = std::get_if<FlowForgeFailure>(&refused.error());
        require(failure && failure->message == "initializer refused" && failure->node_id == entry.id.value);
        require(graph.node(entry.id) == original && graph.pin(id) == original_pin);
        require(graph.pin(id)->default_value.get<std::int32_t>() == 97);
        require(graph.layout().find(entry.id) && *graph.layout().find(entry.id) == placement.layout);
        node.payload.get<InitialPayload>()->reject = false;
        node.payload.get<InitialPayload>()->wrong_type = true;
        auto incompatible = FlowGraphEdit::prepare(graph, change);
        require(!incompatible && initializer_calls == 3);
        failure = std::get_if<FlowForgeFailure>(&incompatible.error());
        require(failure && failure->message == "pin initializer returned an incompatible value");
        require(graph.node(entry.id) == original && graph.pin(id) == original_pin);
        require(graph.topology().nodes().size() == 1 && graph.topology().pins().size() == 1);
        require(graph.pin(id)->default_value.get<std::int32_t>() == 97);
        require(graph.layout().find(entry.id) && *graph.layout().find(entry.id) == placement.layout);

        // A successful earlier initializer cannot leak a partially inserted node if a later one fails.
        auto valid_node = makeNode(catalog, "tests.initializer");
        const std::array mixed{FlowNodeEntry{NodeId{31}, &valid_node, {}}, entry};
        change.insert = mixed;
        auto mixed_failure = FlowGraphEdit::prepare(graph, change);
        require(!mixed_failure && initializer_calls == 5);
        require(graph.node(NodeId{31}) == nullptr && graph.node(entry.id) == original);
        require(graph.pin(id) == original_pin && graph.pin(id)->default_value.get<std::int32_t>() == 97);
        require(graph.layout().find(entry.id) && *graph.layout().find(entry.id) == placement.layout);
    }

    void controls(FlowNodeCatalog& catalog) noexcept
    {
        static_assert(!std::is_polymorphic_v<ForLoopPayload>);
        static_assert(!std::is_polymorphic_v<SequencePayload>);
        const auto registrations = controlNodeRegistrations();
        require(catalog.add(registrations).has_value());
        FlowGraph graph;
        const auto for_loop = graph.addNode(makeNode(catalog, "lux.flow.for_loop"));
        const auto while_loop = graph.addNode(makeNode(catalog, "lux.flow.while_loop"));
        const auto branch = graph.addNode(makeNode(catalog, "lux.flow.branch"));
        require(for_loop.has_value() && while_loop.has_value() && branch.has_value());
        require(graph.pin(pinNamed(graph, *for_loop, "First Index"))->default_value.get<std::int32_t>() == 0);
        require(graph.pin(pinNamed(graph, *for_loop, "Last Index"))->default_value.get<std::int32_t>() == 10);
        require(graph.pin(pinNamed(graph, *while_loop, "Condition"))->default_value.get<bool>());
        require(!graph.pin(pinNamed(graph, *branch, "Condition"))->default_value.get<bool>());
        auto sequence = makeNode(catalog, "lux.flow.sequence");
        sequence.payload.get<SequencePayload>()->additional_outputs = 3;
        const auto sequence_id = graph.addNode(std::move(sequence));
        require(sequence_id.has_value());
        const auto schema = graph.node(*sequence_id)->definition->describePins(graph.node(*sequence_id)->payload);
        require(schema.has_value() && schema->size() == 5);
        for (std::size_t i = 1; i != schema->size(); ++i)
        {
            require((*schema)[i].semantic.value == (std::uint64_t{3} << 56U | i));
        }
        require(!SequencePayload{SIZE_MAX}.describePins());
        require(StartPayload{}.describePins()->size() == 1);
        require(ReturnPayload{}.describePins()->size() == 1);
        require(BreakPayload{}.describePins()->size() == 1);
    }

    // Records the real registration boundary; SSA/region semantics require the separate AOT tests.
    class ExecutionProbe final : public FlowExecutionCompiler
    {
    public:
        std::string action;
        std::vector<graph::PinSemanticId> pins;
        unsigned calls{};
        bool reject{};
        const NativeCallDefinition* native{};
        graph::NodeId callee;
        std::size_t argument_count{};
        const ScriptAbilityPayload* ability{};
        const ScriptEventPayload* event{};
        std::uint64_t variable{};
        const meta::RefField* field{};

        FlowForgeResult<void> branch(
            graph::PinSemanticId condition,
            graph::PinSemanticId true_leg,
            graph::PinSemanticId false_leg
        ) noexcept override
        {
            return record("branch", {condition, true_leg, false_leg});
        }

        FlowForgeResult<void> forLoop(
            graph::PinSemanticId first,
            graph::PinSemanticId last,
            graph::PinSemanticId index,
            graph::PinSemanticId body,
            graph::PinSemanticId completed
        ) noexcept override
        {
            return record("for", {first, last, index, body, completed});
        }

        FlowForgeResult<void> whileLoop(
            graph::PinSemanticId condition,
            graph::PinSemanticId body,
            graph::PinSemanticId completed
        ) noexcept override
        {
            return record("while", {condition, body, completed});
        }

        FlowForgeResult<void> sequence(std::span<const graph::PinSemanticId> values) noexcept override
        {
            return record("sequence", {values.begin(), values.end()});
        }

        FlowForgeResult<void> returnValues(std::span<const graph::PinSemanticId> values) noexcept override
        {
            return record("return", {values.begin(), values.end()});
        }

        FlowForgeResult<void> breakLoop() noexcept override
        {
            return record("break", {});
        }

        FlowForgeResult<void> nativeCall(
            const NativeCallDefinition& definition,
            std::span<const graph::PinSemanticId> arguments,
            graph::PinSemanticId result,
            graph::PinSemanticId completed
        ) noexcept override
        {
            native = &definition;
            std::vector<graph::PinSemanticId> values(arguments.begin(), arguments.end());
            values.push_back(result);
            values.push_back(completed);
            return record("native", std::move(values));
        }

        FlowForgeResult<void> functionCall(
            graph::NodeId target,
            std::span<const graph::PinSemanticId> arguments,
            std::span<const graph::PinSemanticId> results,
            graph::PinSemanticId completed
        ) noexcept override
        {
            callee = target;
            argument_count = arguments.size();
            std::vector<graph::PinSemanticId> values(arguments.begin(), arguments.end());
            values.insert(values.end(), results.begin(), results.end());
            values.push_back(completed);
            return record("function", std::move(values));
        }

        FlowForgeResult<void> abilityCall(
            const ScriptAbilityPayload& value,
            std::span<const graph::PinSemanticId> arguments,
            std::span<const graph::PinSemanticId> results,
            graph::PinSemanticId completed
        ) noexcept override
        {
            ability = &value;
            argument_count = arguments.size();
            std::vector<graph::PinSemanticId> values(arguments.begin(), arguments.end());
            values.insert(values.end(), results.begin(), results.end());
            values.push_back(completed);
            return record("ability", std::move(values));
        }

        FlowForgeResult<void> eventWait(
            const ScriptEventPayload& value,
            graph::PinSemanticId payload,
            graph::PinSemanticId completed
        ) noexcept override
        {
            event = &value;
            return record("event", {payload, completed});
        }

        FlowForgeResult<void> storeVariable(
            std::uint64_t target,
            graph::PinSemanticId value,
            graph::PinSemanticId result,
            graph::PinSemanticId completed
        ) noexcept override
        {
            variable = target;
            return record("store-variable", {value, result, completed});
        }

        FlowForgeResult<void> storeField(
            const meta::RefField& target,
            graph::PinSemanticId object,
            graph::PinSemanticId value,
            graph::PinSemanticId result,
            graph::PinSemanticId completed
        ) noexcept override
        {
            field = &target;
            return record("store-field", {object, value, result, completed});
        }

    private:
        FlowForgeResult<void> record(std::string name, std::vector<graph::PinSemanticId> values) noexcept
        {
            ++calls;
            action = std::move(name);
            pins = std::move(values);
            if (reject)
            {
                return cxx::unexpected(
                    FlowForgeFailure{EFlowForgeError::LOWERING_FAILED, "backend refused region", 987, 123}
                );
            }
            return {};
        }
    };

    void compilation(FlowNodeCatalog& catalog) noexcept
    {
        ExecutionProbe compiler;
        const auto data = [](std::uint64_t ordinal) noexcept
        { return graph::PinSemanticId{(std::uint64_t{4} << 56U) | (ordinal + 1)}; };
        const auto leg = [](std::uint64_t ordinal) noexcept
        { return graph::PinSemanticId{(std::uint64_t{3} << 56U) | (ordinal + 1)}; };
        for (const auto& entry : controlNodeRegistrations())
        {
            const auto node = makeNode(catalog, entry.identity.canonical_name);
            require(node.definition->hasExecutionCompiler());
            const auto encoded = node.definition->encode(node.payload);
            require(encoded.has_value());
            auto decoded = node.definition->decode(*encoded);
            require(decoded.has_value());
            auto cloned = decoded->clone();
            require(cloned.has_value());
            const auto reencoded = node.definition->encode(*cloned);
            require(reencoded.has_value() && *reencoded == *encoded);
            require(!node.definition->decode("invalid"));
            const auto before = compiler.calls;
            const auto compiled = node.definition->compileExecution(*cloned, compiler);
            if (entry.identity.canonical_name == "lux.flow.start")
            {
                require(!compiled && compiled.error().message == "entry node reached mid-chain");
                require(compiled.error().code == EFlowForgeError::GRAPH_INVALID);
                require(compiler.calls == before);
                continue;
            }
            require(compiled.has_value() && compiler.calls == before + 1);
            if (compiler.action == "branch")
            {
                require(compiler.pins == std::vector{data(1), leg(0), leg(1)});
            }
            else if (compiler.action == "for")
            {
                const graph::PinSemanticId index{(std::uint64_t{5} << 56U) | 3};
                require(compiler.pins == std::vector{data(1), data(2), index, leg(0), leg(1)});
            }
            else if (compiler.action == "while")
            {
                require(compiler.pins == std::vector{data(1), leg(0), leg(1)});
            }
            else if (compiler.action == "sequence")
            {
                require(compiler.pins == std::vector{leg(0)});
            }
            else
            {
                require(compiler.pins.empty() && (compiler.action == "return" || compiler.action == "break"));
            }
        }
        auto sequence = makeNode(catalog, "lux.flow.sequence");
        sequence.payload.get<SequencePayload>()->additional_outputs = 3;
        require(sequence.definition->compileExecution(sequence.payload, compiler).has_value());
        require(compiler.pins == std::vector{leg(0), leg(1), leg(2), leg(3)});
        const auto encoded = sequence.definition->encode(sequence.payload);
        require(encoded.has_value() && *encoded == "3");
        const auto decoded = sequence.definition->decode(*encoded);
        require(decoded.has_value() && decoded->get<SequencePayload>()->additional_outputs == 3);
        compiler.reject = true;
        const auto refused = sequence.definition->compileExecution(sequence.payload, compiler);
        require(!refused && refused.error().message == "backend refused region");
        require(refused.error().node_id == 987 && refused.error().pin_id == 123);
        const auto calls = compiler.calls;
        sequence.payload.get<SequencePayload>()->additional_outputs = SIZE_MAX;
        require(!sequence.definition->compileExecution(sequence.payload, compiler));
        require(compiler.calls == calls);

        // Open registration uses the same execution contract, without a backend type/name branch.
        auto external = controlNodeRegistrations()[1];
        external.identity = {graph::nodeTypeId("tests.external.branch"), "tests.external.branch", 1};
        require(catalog.add({&external, 1}).has_value());
        auto custom = makeNode(catalog, "tests.external.branch");
        compiler.reject = false;
        require(custom.definition->compileExecution(custom.payload, compiler).has_value());
        require(compiler.action == "branch" && compiler.pins == std::vector{data(1), leg(0), leg(1)});
        auto wrong = makeNode(catalog, "lux.flow.start");
        require(!custom.definition->compileExecution(wrong.payload, compiler));

        FlowNodeCatalog isolated;
        external.compile = registration<BranchPayload>("tests.unused").compile;
        require(!isolated.add({&external, 1})); // Neither dual nor missing compiler is a valid contract.
        external.compile = nullptr;
        external.compile_execution = nullptr;
        require(!isolated.add({&external, 1}));
        const std::array mixed{controlNodeRegistrations()[0], external};
        require(!isolated.add(mixed));
        require(isolated.find(mixed.front().identity.id) == nullptr);
        auto reassigned = controlNodeRegistrations()[1];
        reassigned.compile_execution = controlNodeRegistrations()[6].compile_execution;
        require(!isolated.add({&reassigned, 1}));
        require(isolated.find(external.identity.id) == nullptr);
    }

    void functionCompilation() noexcept
    {
        FlowNodeCatalog catalog;
        const auto registrations = functionNodeRegistrations();
        require(catalog.add(registrations).has_value());
        ExecutionProbe compiler;
        const auto* integer = &meta::ref_type_of_v<std::int32_t>;
        FunctionPayload signature{{{integer, "a"}, {integer, "b"}}, {{integer, "x"}, {integer, "y"}}};
        auto definition = makePayloadNode(catalog, "lux.flow.function", signature);
        auto call = makePayloadNode(
            catalog,
            "lux.flow.function_call",
            FunctionCallPayload{NodeId{83}, signature.arguments, signature.results}
        );
        auto returned =
            makePayloadNode(catalog, "lux.flow.function_return", FunctionReturnPayload{NodeId{83}, signature.results});
        auto event = makePayloadNode(catalog, "lux.flow.event", EventEntryPayload{signature.arguments});
        const graph::PinSemanticId input1{(std::uint64_t{4} << 56U) | 2};
        const graph::PinSemanticId input2{(std::uint64_t{4} << 56U) | 3};
        const graph::PinSemanticId output1{(std::uint64_t{5} << 56U) | 2};
        const graph::PinSemanticId output2{(std::uint64_t{5} << 56U) | 3};
        const graph::PinSemanticId completed{(std::uint64_t{3} << 56U) | 1};
        require(call.definition->compileExecution(call.payload, compiler).has_value());
        require(compiler.callee == NodeId{83} && compiler.argument_count == 2);
        require(compiler.pins == std::vector{input1, input2, output1, output2, completed});
        require(returned.definition->compileExecution(returned.payload, compiler).has_value());
        require(compiler.action == "return" && compiler.pins == std::vector{input1, input2});
        for (const auto* entry : {&definition, &event})
        {
            const auto before = compiler.calls;
            const auto refused = entry->definition->compileExecution(entry->payload, compiler);
            require(!refused && refused.error().code == EFlowForgeError::GRAPH_INVALID);
            require(refused.error().message == "entry node reached mid-chain" && compiler.calls == before);
        }
        compiler.reject = true;
        const auto refused = call.definition->compileExecution(call.payload, compiler);
        require(!refused && refused.error().node_id == 987 && refused.error().pin_id == 123);
        require(refused.error().message == "backend refused region");
        compiler.reject = false;
        auto cloned = call.clone();
        require(cloned.has_value());
        call.payload.get<FunctionCallPayload>()->arguments[0].type = nullptr;
        const auto before = compiler.calls;
        require(!call.definition->compileExecution(call.payload, compiler) && compiler.calls == before);

        // Real registrations validate the complete candidate, not insertion order or pointer identity.
        FlowGraph graph;
        const std::array entries{
            FlowNodeEntry{NodeId{85}, &*cloned, {}},
            FlowNodeEntry{NodeId{87}, &returned, {}},
            FlowNodeEntry{NodeId{83}, &definition, {}}
        };
        auto edit = FlowGraphEdit::prepare(graph, {.insert = entries});
        require(edit.has_value());
        edit->commit();
        require(!graph.removeNode(NodeId{83}));
        const auto* original = graph.node(NodeId{83});
        auto mismatched = makePayloadNode(catalog, "lux.flow.function", signature);
        mismatched.payload.get<FunctionPayload>()->results[1].type = &meta::ref_type_of_v<float>;
        const FlowNodeEntry replace{NodeId{83}, &mismatched, {}};
        const NodeId removed{83};
        auto invalid = FlowGraphEdit::prepare(graph, {.insert = {&replace, 1}, .erase = {&removed, 1}});
        require(!invalid && graph.node(removed) == original && graph.topology().nodes().size() == 3);
        mismatched.payload.get<FunctionPayload>()->results = signature.results;
        mismatched.payload.get<FunctionPayload>()->arguments[0].name = "renamed";
        auto valid = FlowGraphEdit::prepare(graph, {.insert = {&replace, 1}, .erase = {&removed, 1}});
        require(valid.has_value());
        valid->commit();
        const auto& stored = *graph.node(NodeId{85});
        require(stored.definition->compileExecution(stored.payload, compiler).has_value());
        require(compiler.callee == removed);

        FlowNodeCatalog refused_catalog;
        auto replaced = registrations[2];
        replaced.compile_execution = registrations[1].compile_execution;
        require(!refused_catalog.add({&replaced, 1}));
        require(refused_catalog.find(replaced.identity.id) == nullptr);
    }

    void scriptCompilation() noexcept
    {
        FlowNodeCatalog catalog;
        const std::array registrations{scriptAbilityRegistration(), scriptEventRegistration()};
        require(catalog.add(registrations).has_value());
        auto ability = makeNode(catalog, "lux.flow.ability_call");
        std::string contract{"tests.runtime"};
        const script::ScriptAbilityValueDescription value{
            semantic::typeId("lux.i32"),
            "lux.i32",
            semantic::EValuePass::VALUE,
            static_cast<std::uint8_t>(semantic::EAbiKind::I32),
            4,
            4,
            script::EScriptAbilityValueLifetime::OWNED_VALUE
        };
        const std::array parameters{script::ScriptAbilityParameterDescription{"input", value}};
        const std::array results{value};
        *ability.payload.get<ScriptAbilityPayload>() = ScriptAbilityPayload{ScriptAbilityNodeDescription{
            script::ScriptApiContractIdView{contract},
            script::ScriptApiMethodIdView{"wait"},
            "Runtime",
            "Wait",
            3,
            71,
            script::EScriptAbilityReceiverKind::NONE,
            script::EScriptApiMethodKind::ASYNC_OPERATION,
            parameters,
            results
        }};
        auto copy = ability.clone();
        require(copy.has_value());
        ability = {};
        contract.assign(contract.size(), '#');
        ExecutionProbe compiler;
        require(copy->definition->compileExecution(copy->payload, compiler).has_value());
        require(compiler.action == "ability" && compiler.argument_count == 1);
        require(compiler.ability->contract().name() == "tests.runtime");
        require(compiler.ability->expectedSchemaHash() == 71);
        const graph::PinSemanticId input{(std::uint64_t{4} << 56U) | 2};
        const graph::PinSemanticId output{(std::uint64_t{5} << 56U) | 2};
        const graph::PinSemanticId completed{(std::uint64_t{3} << 56U) | 1};
        require(compiler.pins == std::vector{input, output, completed});

        auto event = makeNode(catalog, "lux.flow.event_wait");
        script::ScriptEventSourceDescription source;
        source.system_name = "tests.system";
        source.event_name = "changed";
        source.payload = {"lux.i32", value.type_id, value.abi_kind, 4, 4};
        *event.payload.get<ScriptEventPayload>() = ScriptEventPayload{source};
        auto event_copy = event.clone();
        require(event_copy.has_value());
        event = {};
        source.event_name = "overwritten";
        require(event_copy->definition->compileExecution(event_copy->payload, compiler).has_value());
        require(compiler.action == "event" && compiler.event->source().event_name == "changed");
        require(compiler.pins == std::vector{output, completed});
        compiler.reject = true;
        for (const auto* node : {&*copy, &*event_copy})
        {
            const auto result = node->definition->compileExecution(node->payload, compiler);
            require(!result && result.error().code == EFlowForgeError::LOWERING_FAILED);
            require(result.error().node_id == 987 && result.error().pin_id == 123);
        }
        FlowNodeCatalog isolated;
        auto reassigned = registrations[0];
        reassigned.compile_execution = registrations[1].compile_execution;
        require(!isolated.add({&reassigned, 1}));
    }

    void palette() noexcept
    {
        static_assert(!std::is_default_constructible_v<NodeRegistry>);
        const auto invalid = NodeRegistry::create(object::CodeLease::plugin({}));
        require(!invalid && invalid.error().code == EFlowForgeError::INVALID_DESCRIPTION);
        FlowGraph graph;
        std::weak_ptr<const void> provider;
        {
            auto code = std::make_shared<int>(7);
            provider = code;
            auto created_palette = NodeRegistry::create(object::CodeLease::plugin(std::move(code)));
            require(created_palette.has_value());
            auto& palette = **created_palette;
            require(palette.nodeCreators().size() == 33);
            for (const auto& recipe : palette.nodeCreators())
            {
                auto node = recipe->creator();
                require(node.has_value() && node->creator == recipe->name);
                require(node->definition != nullptr && !node->name.empty());
                auto inserted = graph.addNode(std::move(*node));
                require(inserted.has_value());
            }
            auto* integer = palette.findNodeByName("Add (Int)");
            auto* real = palette.findNodeByName("Add (Float)");
            require(integer && real && palette.findNodeByCategory("Compare"));
            require(palette.findNodeByName("missing") == nullptr);
            require(palette.findNodeByCategory("missing") == nullptr);
            auto a = integer->creator();
            auto b = real->creator();
            require(a.has_value() && b.has_value() && a->name == "Add" && b->name == "Add");
            require(a->definition == b->definition);
            auto duplicate = std::make_unique<NodeCreateInfo>();
            duplicate->name = "Add (Int)";
            duplicate->creator = []() noexcept -> FlowForgeResult<FlowNode>
            { return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "not invoked"}); };
            require(!palette.registerNode(std::move(duplicate)));
            require(!palette.registerNode(std::make_unique<NodeCreateInfo>()));
            require(palette.nodeCreators().size() == 33);
        }
        // Real registered definitions and payloads survive the creation recipes.
        require(!provider.expired());
        require(graph.topology().nodes().size() == 33);
        for (const auto& [id, node] : graph.nodes())
        {
            require(node->definition->describePins(node->payload).has_value());
            require(node->clone().has_value());
        }
        graph = FlowGraph{};
        require(provider.expired());

        std::optional<FlowNode> native;
        {
            auto created_palette = NodeRegistry::create();
            require(created_palette.has_value());
            auto& palette = **created_palette;
            std::string name = "palette_compute";
            std::string full_name = "tests.palette_compute";
            std::string parameter = "input";
            auto function = std::make_unique<meta::RefFunction>();
            auto& signature = function->invokable;
            signature.name = name;
            signature.full_name = full_name;
            signature.return_type = meta::ref_type_of_v<std::int32_t>;
            signature.parameters.push_back(
                {parameter, signature.return_type, signature.return_type.name, signature.return_type.hash, false}
            );
            signature.invoker = [](void*, void**, void*) {}; // Reflection fixture, not native execution evidence.
            meta::ReflectionRegistry::instance().registerFunction({full_name, {}}, std::move(function));
            require(
                palette.populateFromReflection(meta::ReflectionRegistry::instance(), object::CodeLease::builtin()) > 0
            );
            auto* recipe = palette.findNodeByName(name);
            require(recipe != nullptr);
            name.assign(name.size(), '#');
            parameter.assign(parameter.size(), '#');
            auto created = recipe->creator();
            require(created.has_value());
            native.emplace(std::move(*created));
        }
        const auto* call = native->payload.get<NativeCallPayload>();
        require(call && call->definition->signature().name == "palette_compute");
        require(call->definition->signature().parameters[0].name == "input");
        require(native->creator == "palette_compute" && native->name == "palette_compute");
        ExecutionProbe compiler;
        require(native->definition->compileExecution(native->payload, compiler).has_value());
        require(compiler.action == "native" && compiler.native == call->definition.get());
        const graph::PinSemanticId input{(std::uint64_t{4} << 56U) | 2};
        const graph::PinSemanticId output{(std::uint64_t{5} << 56U) | 2};
        const graph::PinSemanticId completed{(std::uint64_t{3} << 56U) | 1};
        require(compiler.pins == std::vector{input, output, completed});
        compiler.reject = true;
        const auto rejected = native->definition->compileExecution(native->payload, compiler);
        require(!rejected && rejected.error().message == "backend refused region");
        auto clone = native->clone();
        require(clone.has_value());
        native.reset();
        compiler.reject = false;
        require(clone->definition->compileExecution(clone->payload, compiler).has_value());
        require(compiler.native->signature().name == "palette_compute");
        auto method = NativeCallDefinition::
            create(compiler.native->signature(), object::CodeLease::builtin(), &meta::ref_type_of_v<void*>);
        require(method.has_value());
        clone->payload.get<NativeCallPayload>()->definition = std::move(*method);
        require(clone->definition->compileExecution(clone->payload, compiler).has_value());
        const graph::PinSemanticId self{(std::uint64_t{4} << 56U) | 3};
        require(compiler.pins == std::vector{self, input, output, completed});
        require(compiler.native->receiver() != nullptr);
        clone->payload.get<NativeCallPayload>()->definition.reset();
        const auto calls = compiler.calls;
        const auto missing = clone->definition->compileExecution(clone->payload, compiler);
        require(!missing && missing.error().message == "native call has no definition" && compiler.calls == calls);
    }

    class MemoryProbe final : public FlowValueCompiler
    {
    public:
        std::vector<const meta::RefType*> values{&meta::ref_type_of_v<void*>};
        std::uint64_t variable{};
        const meta::RefField* field{};
        unsigned reads{};
        bool reject{};
        bool wrong_type{};

        const meta::RefType* type(FlowValue value) const noexcept override
        {
            return value < values.size() ? values[value] : nullptr;
        }

        FlowForgeResult<FlowValue> readVariable(std::uint64_t id) noexcept override
        {
            variable = id;
            return record(meta::ref_type_of_v<std::int32_t>);
        }

        FlowForgeResult<FlowValue> readField(const meta::RefField& value, FlowValue object) noexcept override
        {
            require(object == 0);
            field = &value;
            return record(value.type);
        }

    private:
        FlowForgeResult<FlowValue>
        emitScalarImpl(EScalarInstruction, std::span<const FlowValue>, const meta::RefType&) noexcept override
        {
            require(false);
            std::terminate();
        }

        FlowForgeResult<FlowValue> record(const meta::RefType& type) noexcept
        {
            ++reads;
            if (reject)
            {
                return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "memory read refused", 91, 73});
            }
            values.push_back(wrong_type ? &meta::ref_type_of_v<float> : &type);
            return static_cast<FlowValue>(values.size() - 1);
        }
    };

    void objectCompilation(FlowNodeCatalog& catalog) noexcept
    {
        const auto* integer = &meta::ref_type_of_v<std::int32_t>;
        meta::RefClass owner;
        owner.type = meta::ref_type_of_v<void*>;
        owner.fields.push_back(meta::RefField{"count", *integer});
        MemoryProbe compiler;
        const std::array input{FlowValue{0}};
        auto field = makePayloadNode(catalog, "lux.flow.get_field", GetFieldPayload{&owner, &owner.fields[0]});
        auto variable = makePayloadNode(catalog, "lux.flow.get_variable", GetVariablePayload{31, integer});
        for (const auto* node : {&field, &variable})
        {
            require(node->definition->valueEvaluation() == EFlowValueEvaluation::READS_STATE);
            require(!node->definition->hasExecutionCompiler());
            const auto arguments = node == &field ? std::span{input} : std::span<const FlowValue>{};
            const auto first = node->definition->compile(node->payload, arguments, compiler);
            const auto second = node->definition->compile(node->payload, arguments, compiler);
            require(first.has_value() && second.has_value() && first->front() != second->front());
            compiler.reject = true;
            const auto refused = node->definition->compile(node->payload, arguments, compiler);
            require(!refused && refused.error().message == "memory read refused");
            require(refused.error().node_id == 91 && refused.error().pin_id == 73);
            compiler.reject = false;
            compiler.wrong_type = true;
            const auto wrong = node->definition->compile(node->payload, arguments, compiler);
            require(!wrong && wrong.error().message == "node compiler returned an invalid output value");
            compiler.wrong_type = false;
        }
        require(compiler.variable == 31 && compiler.field == &owner.fields[0] && compiler.reads == 8);
        field.payload.get<GetFieldPayload>()->field = nullptr;
        require(!field.definition->compile(field.payload, input, compiler) && compiler.reads == 8);

        ExecutionProbe execution;
        auto store = makePayloadNode(catalog, "lux.flow.set_variable", SetVariablePayload{31, integer});
        const graph::PinSemanticId input1{(std::uint64_t{4} << 56U) | 2};
        const graph::PinSemanticId input2{(std::uint64_t{4} << 56U) | 3};
        const graph::PinSemanticId output1{(std::uint64_t{5} << 56U) | 2};
        const graph::PinSemanticId completed{(std::uint64_t{3} << 56U) | 1};
        require(store.definition->compileExecution(store.payload, execution).has_value());
        require(execution.variable == 31 && execution.pins == std::vector{input1, output1, completed});
        auto set_field = makePayloadNode(catalog, "lux.flow.set_field", SetFieldPayload{&owner, &owner.fields[0]});
        require(set_field.definition->compileExecution(set_field.payload, execution).has_value());
        require(execution.field == &owner.fields[0]);
        require(execution.pins == std::vector{input1, input2, output1, completed});
        auto unsupported_read = makePayloadNode(catalog, "lux.flow.get_object", GetObjectPayload{integer});
        const auto read = unsupported_read.definition->compile(unsupported_read.payload, {}, compiler);
        require(!read && read.error().code == EFlowForgeError::GRAPH_INVALID);
        require(read.error().message == "no pure lowering registered for this node" && compiler.reads == 8);
        auto unsupported_store = makePayloadNode(catalog, "lux.flow.set_object", SetObjectPayload{integer});
        const auto write = unsupported_store.definition->compileExecution(unsupported_store.payload, execution);
        require(!write && write.error().code == EFlowForgeError::GRAPH_INVALID);
        require(write.error().message == "no lowering registered" && execution.calls == 2);

        auto changed = objectNodeRegistrations()[2];
        changed.value_evaluation = EFlowValueEvaluation::PURE;
        FlowNodeCatalog isolated;
        require(!isolated.add({&changed, 1}));
        changed.identity = {graph::nodeTypeId("tests.external.memory"), "tests.external.memory", 1};
        changed.value_evaluation = static_cast<EFlowValueEvaluation>(255);
        require(!isolated.add({&changed, 1}));
        changed = objectNodeRegistrations()[3];
        changed.identity = {graph::nodeTypeId("tests.external.store"), "tests.external.store", 1};
        changed.value_evaluation = EFlowValueEvaluation::READS_STATE;
        require(!isolated.add({&changed, 1}));
    }

    void objects(FlowNodeCatalog& catalog) noexcept
    {
        require(catalog.add(objectNodeRegistrations()).has_value());
        static_assert(!std::is_polymorphic_v<GetFieldPayload>);
        static_assert(!std::is_polymorphic_v<SetVariablePayload>);
        const auto* integer = &meta::ref_type_of_v<std::int32_t>;
        meta::RefClass owner;
        owner.name = "Object";
        owner.type = meta::ref_type_of_v<void*>;
        owner.fields.push_back(meta::RefField{"count", *integer});
        FlowGraph graph; // The metadata environment outlives every pin and payload.
        auto get =
            graph.addNode(makePayloadNode(catalog, "lux.flow.get_field", GetFieldPayload{&owner, &owner.fields[0]}));
        auto set =
            graph.addNode(makePayloadNode(catalog, "lux.flow.set_field", SetFieldPayload{&owner, &owner.fields[0]}));
        require(get.has_value() && set.has_value());
        const auto object = pinNamed(graph, *get, "Object");
        require(!graph.pin(object)->default_value.isValid());
        const auto value = pinNamed(graph, *set, "count");
        require(graph.pin(value)->allow_default && graph.pin(value)->default_value.get<std::int32_t>() == 0);
        require(graph.node(*set)->definition->describePins(graph.node(*set)->payload)->size() == 5);
        require(!GetFieldPayload{}.describePins());
        require(!SetFieldPayload{&owner, nullptr}.describePins());
        require(!GetObjectPayload{}.describePins() && !SetObjectPayload{}.describePins());

        // Original SetObject stores a zero even though its default-value editor is disabled.
        const auto object_set =
            graph.addNode(makePayloadNode(catalog, "lux.flow.set_object", SetObjectPayload{integer}));
        require(object_set.has_value());
        const auto object_value = pinNamed(graph, *object_set, "Value");
        require(!graph.pin(object_value)->allow_default);
        require(graph.pin(object_value)->default_value.get<std::int32_t>() == 0);

        const auto variable = graph.addVariable("counter", integer, meta::RuntimeObject{std::int32_t{12}});
        require(variable != 0);
        auto reader = makePayloadNode(catalog, "lux.flow.get_variable", GetVariablePayload{variable, integer});
        auto writer = makePayloadNode(catalog, "lux.flow.set_variable", SetVariablePayload{variable, integer});
        const auto read_id = graph.addNode(std::move(reader));
        const auto write_id = graph.addNode(std::move(writer));
        require(read_id.has_value() && write_id.has_value());
        graph.findVariable(variable)->name = "renamed";
        FlowGraphChange no_change;
        require(FlowGraphEdit::prepare(graph, no_change).has_value());
        const auto* original_reader = graph.node(*read_id);
        auto wrong = makePayloadNode(
            catalog,
            "lux.flow.get_variable",
            GetVariablePayload{variable, &meta::ref_type_of_v<float>}
        );
        const FlowNodeEntry replacement{*read_id, &wrong, {}};
        FlowGraphChange change;
        change.erase = {&*read_id, 1};
        change.insert = {&replacement, 1};
        auto refused = FlowGraphEdit::prepare(graph, change);
        require(!refused && graph.node(*read_id) == original_reader);
        const auto* failure = std::get_if<FlowForgeFailure>(&refused.error());
        require(failure && failure->message == "variable node type differs from its definition");
        wrong.payload.get<GetVariablePayload>()->variable = UINT64_MAX;
        auto missing = FlowGraphEdit::prepare(graph, change);
        require(!missing && graph.node(*read_id) == original_reader);
        failure = std::get_if<FlowForgeFailure>(&missing.error());
        require(failure && failure->message == "graph variable not found");
        require(graph.findVariable(variable)->default_value.get<std::int32_t>() == 12);
    }
} // namespace

int main()
{
    meta::meta_module_init();
    FlowNodeCatalog catalog;
    controls(catalog);
    compilation(catalog);
    functionCompilation();
    scriptCompilation();
    palette();
    preparation(catalog);
    objects(catalog);
    objectCompilation(catalog);
    std::puts("PASS: real control registrations/codecs/compile contracts, defaults, restoration and atomic refusal");
}
