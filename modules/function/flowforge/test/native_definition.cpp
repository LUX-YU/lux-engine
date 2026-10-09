#include "NativeGraphFixture.hpp"
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/flowforge/graph/NodeRegistry.hpp>
#include <lux/engine/meta/MetaCompat.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool value, std::source_location at = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "native signature contract failed at %u\n", at.line());
            std::exit(42);
        }
    }

    struct Input final
    {
        std::string name{"compute"};
        std::string full_name{"vendor.compute"};
        std::string signature{"int(int)"};
        std::string parameter{"argument"};
        std::string type_name{"int"};
        meta::RefFunction function;

        Input()
        {
            auto type = meta::ref_type_of_v<int>;
            type.name = type_name;
            function.invokable.name = name;
            function.invokable.full_name = full_name;
            function.invokable.type_signature = signature;
            function.invokable.return_type = type;
            function.invokable.parameters.push_back({parameter, type, type_name, type.hash, false});
        }
    };

    const FlowNode* replaceNative(FlowGraph& graph, NodeId id, native_fixture::Definition definition)
    {
        const auto* old = graph.node(id);
        auto candidate = native_fixture::candidate(id, std::move(definition));
        native_fixture::preserveExecutionPins(candidate, graph);
        const std::array erase{id};
        const std::array<FlowNodeEntry, 1> insert{{{id, &candidate.value, candidate.pins}}};
        std::vector<graph::GraphLayoutEntry> layout;
        if (const auto* saved = graph.layout().find(id))
        {
            layout.push_back({id, *saved});
        }
        auto plan = FlowGraphEdit::prepare(graph, {.insert = insert, .erase = erase, .place = layout});
        require(plan.has_value() && candidate.value.definition && graph.node(id) == old);
        const auto* old_native = native_fixture::native(*old).definition.get();
        plan->commit();
        auto removed = plan->takeRemoved();
        require(removed.size() == 1 && native_fixture::native(removed.front().value).definition.get() == old_native);
        const auto* result = graph.node(id);
        require(
            result && native_fixture::native(*result).definition == native_fixture::native(candidate.value).definition
        );
        for (const auto& pin : candidate.pins)
        {
            if (pin.value.role == EFlowPinRole::EXECUTION)
            {
                require(graph.pinId(id, pin.record.semantic) == pin.record.id);
            }
        }
        // Old RuntimeObject defaults die before their definition/code. Disposal is outside commit.
        removed.clear();
        return result;
    }

    void exhaustedReplacement()
    {
        Input input;
        auto definition = NativeCallDefinition::create(input.function.invokable, object::CodeLease::builtin());
        require(definition.has_value());
        FlowGraph graph;
        auto original = native_fixture::candidate({1}, *definition);
        require(original.pins.size() == 4);
        const std::array<PinId, 4> ids{{{1}, {2}, {3}, {UINT64_MAX}}};
        for (std::size_t i = 0; i != ids.size(); ++i)
        {
            original.pins[i].record.id = ids[i];
        }
        const std::array<FlowNodeEntry, 1> first{{{{1}, &original.value, original.pins}}};
        auto insertion = FlowGraphEdit::prepare(graph, {.insert = first});
        require(insertion.has_value());
        insertion->commit();
        const auto* previous = graph.node({1});
        const asset::AssetId asset{std::array<std::uint8_t, 16>{1}};
        auto before = captureFlowSource(asset, "native", graph);
        require(before.has_value());
        auto candidate = native_fixture::candidate({1}, *definition);
        native_fixture::preserveExecutionPins(candidate, graph);
        const std::array<NodeId, 1> erase{{{1}}};
        const std::array<FlowNodeEntry, 1> insert{{{{1}, &candidate.value, candidate.pins}}};
        auto refused = FlowGraphEdit::prepare(graph, {.insert = insert, .erase = erase});
        require(!refused && candidate.value.definition && graph.node({1}) == previous);
        auto after = captureFlowSource(asset, "native", graph);
        require(after.has_value() && *before == *after);
        require(native_fixture::pin(graph, {1}, EFlowPinRole::DATA, graph::EPinDirection::INPUT) == PinId{2});
        require(native_fixture::pin(graph, {1}, EFlowPinRole::DATA, graph::EPinDirection::OUTPUT) == PinId{UINT64_MAX});
        std::puts("PASS native schema exhaustion: refused candidate, source/defaults/identities intact");
    }

    void check(const FlowGraph& graph, NodeId id)
    {
        const auto& info = native_fixture::native(*graph.node(id)).definition->signature();
        require(info.name == "compute");
        require(info.full_name == "vendor.compute");
        require(info.type_signature == "int(int)");
        require(info.return_type.name == "int");
        require(info.parameters.size() == 1U);
        require(info.parameters[0].name == "argument");
        require(info.parameters[0].type.name == "int");
        require(info.parameters[0].value_type_name == "int");
        const auto input = native_fixture::pin(graph, id, EFlowPinRole::DATA, graph::EPinDirection::INPUT);
        require(input.valid());
        require(!native_fixture::pin(graph, id, EFlowPinRole::DATA, graph::EPinDirection::INPUT, 1).valid());
        require(graph.pin(input)->default_value.isValid());
        int initial{};
        std::memcpy(&initial, graph.pin(input)->default_value.data(), sizeof(initial));
        require(initial == 0);
    }

} // namespace

int main()
{
    using namespace lux;
    using namespace lux::flowforge;
    meta::meta_module_init();
    exhaustedReplacement();
    {
        native_fixture::Definition definition;
        {
            Input input;
            auto result = NativeCallDefinition::create(input.function.invokable, object::CodeLease::builtin());
            require(result.has_value());
            definition = std::move(*result);
            input.name.assign(input.name.size(), '#');
            input.function.invokable.parameters.clear();
        }
        auto node = native_fixture::make(definition);
        definition.reset();
        auto copy = node.clone();
        require(copy.has_value());
        require(native_fixture::native(node).definition == native_fixture::native(*copy).definition);
        FlowGraph graph;
        const auto added = graph.addNode(std::move(*copy));
        require(added.has_value());
        check(graph, *added);
        auto captured = captureFlowSource(asset::AssetId{std::array<std::uint8_t, 16>{1}}, "native", graph);
        require(captured.has_value());
        FlowGraph restored;
        {
            Input input;
            const meta::RefFunction* functions[]{&input.function};
            FlowSourceEnvironment environment;
            environment.functions = functions;
            auto candidate = materializeFlowSource(*captured, environment);
            require(candidate.has_value());
            restored = std::move(*candidate);
        }
        auto recaptured = captureFlowSource(captured->id, "native", restored);
        require(recaptured.has_value());
        require(*captured == *recaptured);
        const auto restored_id = (*restored.nodes().begin()).first;
        const auto* restored_node = restored.node(restored_id);
        const std::array<graph::GraphLayoutEntry, 1> placement{{{restored_id, {4.0F, 11.0F, true}}}};
        auto placed = FlowGraphEdit::prepare(restored, {.place = placement});
        require(placed.has_value());
        placed->commit();
        restored_node = replaceNative(restored, restored_id, native_fixture::native(*restored_node).definition);
        check(restored, restored_id);
        require(restored.layout().find(restored_id)->x == 4.0F);
        require(restored.layout().find(restored_id)->y == 11.0F);

        int released{};
        native_fixture::Definition method;
        {
            Input input;
            auto owner = std::shared_ptr<int>(
                new int{},
                [&](int* value) noexcept
                {
                    ++released;
                    delete value;
                }
            );
            auto type = meta::ref_type_of_v<int>;
            std::string receiver_name{"Receiver"};
            type.name = receiver_name;
            auto result =
                NativeCallDefinition::create(input.function.invokable, object::CodeLease::plugin(owner), &type);
            require(result.has_value());
            method = std::move(*result);
        }
        restored_node = replaceNative(restored, restored_id, std::move(method));
        require(released == 0);
        require(native_fixture::native(*restored_node).definition->receiver()->name == "Receiver");
        const auto arguments = native_fixture::native(*restored_node).argumentSemantics();
        require(arguments.has_value() && arguments->size() == 2U);
        const auto self = restored.pinId(restored_id, arguments->front());
        require(restored.pin(self)->name == "Self");
        Input replacement;
        auto next = NativeCallDefinition::create(replacement.function.invokable, object::CodeLease::builtin());
        require(next.has_value());
        restored_node = replaceNative(restored, restored_id, std::move(*next));
        require(released == 1);
        require(native_fixture::native(*restored_node).definition->receiver() == nullptr);
        check(restored, restored_id);
        std::optional<FlowNode> palette_node;
        {
            auto created_palette = NodeRegistry::create();
            require(created_palette.has_value());
            auto& palette = **created_palette;
            Input input;
            input.function.invokable.invoker = [](void*, void**, void*) {};
            auto reflected = std::make_unique<meta::RefFunction>(input.function);
            meta::ReflectionRegistry::instance().registerFunction({"vendor.compute", {}}, std::move(reflected));
            require(
                palette.populateFromReflection(meta::ReflectionRegistry::instance(), object::CodeLease::builtin()) > 0U
            );
            auto* creator = palette.findNodeByName("compute");
            require(creator != nullptr);
            input.name.assign(input.name.size(), '#');
            auto created = creator->creator();
            require(created.has_value());
            palette_node = std::move(*created);
        }
        FlowGraph palette_graph;
        auto palette_id = palette_graph.addNode(std::move(*palette_node));
        require(palette_id.has_value());
        check(palette_graph, *palette_id);
        auto recreated = native_fixture::make(native_fixture::native(*palette_graph.node(*palette_id)).definition);
        palette_node.reset();
        require(palette_graph.removeNode(*palette_id).has_value());
        palette_id = palette_graph.addNode(std::move(recreated));
        require(palette_id.has_value());
        check(palette_graph, *palette_id);

        native_fixture::Definition base_call;
        native_fixture::Definition derived_call;
        {
            struct Base
            {
                int value;
            };

            struct Derived : Base
            {
                int extra;
            };

            meta::RefClass base;
            meta::RefClass derived;
            base.type = meta::ref_type_of_v<Base>;
            derived.type = meta::ref_type_of_v<Derived>;
            std::string base_name{"NativeBase"};
            std::string derived_name{"NativeDerived"};
            base.name = base.full_name = base_name;
            derived.name = derived.full_name = derived_name;
            base.type.name = base_name;
            derived.type.name = derived_name;
            base.hash = base.type.hash;
            derived.hash = derived.type.hash;
            base.type.ptr = &base;
            derived.type.ptr = &derived;
            derived.parent_chain.push_back(base.hash);
            meta::RefInvokable info;
            info.name = "record_call";
            info.return_type = derived.type;
            info.parameters.push_back({"value", base.type, base_name, base.hash, false});
            auto first = NativeCallDefinition::create(info, object::CodeLease::builtin());
            require(first.has_value());
            base_call = std::move(*first);
            info.parameters.clear();
            auto second = NativeCallDefinition::create(info, object::CodeLease::builtin());
            require(second.has_value());
            derived_call = std::move(*second);
        }
        auto base_node = native_fixture::make(std::move(base_call));
        auto derived_node = native_fixture::make(std::move(derived_call));
        const auto* base_type = &native_fixture::native(base_node).definition->signature().parameters[0].type;
        const auto* derived_type = &native_fixture::native(derived_node).definition->signature().return_type;
        require(meta::canInitialize(base_type, derived_type));
        require(!meta::canInitialize(derived_type, base_type));
        auto base_id = graph.addNode(std::move(base_node));
        auto derived_id = graph.addNode(std::move(derived_node));
        require(base_id.has_value() && derived_id.has_value());
        require(graph
                    .connect(
                        native_fixture::pin(graph, *derived_id, EFlowPinRole::DATA, graph::EPinDirection::OUTPUT),
                        native_fixture::pin(graph, *base_id, EFlowPinRole::DATA, graph::EPinDirection::INPUT)
                    )
                    .has_value());

        auto invalid = NativeCallDefinition::create({}, object::CodeLease::builtin());
        require(!invalid && invalid.error().code == EFlowForgeError::INVALID_DESCRIPTION);
    }
    meta::meta_module_deinit();
    std::puts("PASS native signature: owning names/types, reconstruction, rebind, source roundtrip and code lease");
}
