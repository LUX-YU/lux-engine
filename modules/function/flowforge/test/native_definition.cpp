#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/NodeRegistry.hpp>
#include <lux/engine/meta/MetaCompat.hpp>

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

    void check(const NativeFuncCall& node)
    {
        require(node.info().name == "compute");
        require(node.info().full_name == "vendor.compute");
        require(node.info().type_signature == "int(int)");
        require(node.info().return_type.name == "int");
        require(node.info().parameters.size() == 1U);
        require(node.info().parameters[0].name == "argument");
        require(node.info().parameters[0].type.name == "int");
        require(node.info().parameters[0].value_type_name == "int");
        require(node.dataInPins().size() == 1U);
        require(node.dataInPins()[0]->validConstant());
        int initial{};
        std::memcpy(&initial, node.dataInPins()[0]->constantData().data(), sizeof(initial));
        require(initial == 0);
    }
} // namespace

int main()
{
    using namespace lux;
    using namespace lux::flowforge;
    meta::meta_module_init();
    {
        NativeFuncCall::Definition definition;
        {
            Input input;
            auto result = NativeCallDefinition::create(input.function.invokable, object::CodeLease::builtin());
            require(result.has_value());
            definition = std::move(*result);
            input.name.assign(input.name.size(), '#');
            input.function.invokable.parameters.clear();
        }
        auto node = std::make_unique<NativeFuncCall>(definition);
        definition.reset();
        check(*node);
        const auto exec_in = node->execInPin().id();
        const auto exec_out = node->execOutPin().id();
        node->reconstruct();
        check(*node);
        require(node->execInPin().id() == exec_in && node->execOutPin().id() == exec_out);

        FlowGraph graph;
        require(graph.addNodes(std::move(node)) != 0U);
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
        auto* restored_node = static_cast<NativeFuncCall*>(restored.nodes().front().node.get());
        restored_node->reconstruct();
        check(*restored_node);

        int released{};
        NativeFuncCall::Definition method;
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
        restored_node->rebind(std::move(method));
        require(released == 0);
        require(restored_node->ownerType()->name == "Receiver");
        require(restored_node->dataInPins().size() == 2U);
        require(restored_node->dataInPins().front()->info().name == "Self");
        Input replacement;
        auto next = NativeCallDefinition::create(replacement.function.invokable, object::CodeLease::builtin());
        require(next.has_value());
        restored_node->rebind(std::move(*next));
        require(released == 1);
        require(restored_node->ownerType() == nullptr);
        check(*restored_node);
        std::unique_ptr<Node> palette_node;
        {
            NodeRegistry palette;
            Input input;
            input.function.invokable.invoker = [](void*, void**, void*) {};
            auto reflected = std::make_unique<meta::RefFunction>(input.function);
            meta::ReflectionRegistry::instance().registerFunction({"vendor.compute", {}}, std::move(reflected));
            require(
                palette.populateFromReflection(meta::ReflectionRegistry::instance(), object::CodeLease::builtin()) > 0U
            );
            const auto* creator = palette.findNodeByName("compute");
            require(creator != nullptr);
            input.name.assign(input.name.size(), '#');
            palette_node = creator->creator();
        }
        auto& native = static_cast<NativeFuncCall&>(*palette_node);
        check(native);
        native.reconstruct();
        check(native);

        NativeFuncCall::Definition base_call;
        NativeFuncCall::Definition derived_call;
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
        auto base_node = std::make_unique<NativeFuncCall>(std::move(base_call));
        auto derived_node = std::make_unique<NativeFuncCall>(std::move(derived_call));
        const auto* base_type = base_node->dataInPins()[0]->info().type;
        const auto* derived_type = derived_node->result().info().type;
        require(meta::canInitialize(base_type, derived_type));
        require(!meta::canInitialize(derived_type, base_type));
        auto* base_pointer = base_node.get();
        auto* derived_pointer = derived_node.get();
        require(graph.addNodes(std::move(base_node)) != 0U);
        require(graph.addNodes(std::move(derived_node)) != 0U);
        require(
            graph.connect(
                *graph.findPin(derived_pointer->result().id()),
                *graph.findPin(base_pointer->dataInPins()[0]->id())
            ) == ELinkError::SUCCESS
        );

        auto invalid = NativeCallDefinition::create({}, object::CodeLease::builtin());
        require(!invalid && invalid.error().code == EFlowForgeError::INVALID_DESCRIPTION);
    }
    meta::meta_module_deinit();
    std::puts("PASS native signature: owning names/types, reconstruction, rebind, source roundtrip and code lease");
}
