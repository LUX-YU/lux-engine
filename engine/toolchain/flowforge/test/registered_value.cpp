#include "registered_value_definition.hpp"
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/flowforge/ScalarNodes.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/function/script/native/NativeModule.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;
    using namespace flow_test;

    void require(bool value, std::source_location location = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "registered value failed at %u\n", location.line());
            std::abort();
        }
    }

    template <class T, class... Args> T& add(FlowGraph& graph, Args&&... args)
    {
        auto owner = std::make_unique<T>(std::forward<Args>(args)...);
        auto& node = *owner;
        require(graph.addNode(std::move(owner)).valid());
        return node;
    }

    void link(FlowGraph& graph, const Pin& from, const Pin& to)
    {
        require(
            graph.connect(*graph.findPin(graph.pinId(&from)), *graph.findPin(graph.pinId(&to))) == ELinkError::SUCCESS
        );
    }

    FlowGraph example(bool reject, std::shared_ptr<const FlowNodeType> type)
    {
        FlowGraph graph;
        const auto* integer = &meta::ref_type_of_v<int>;
        auto initial = meta::RuntimeObject::defaultOf(*integer);
        require(initial.has_value());
        const auto variable = graph.addVariable("result", integer, std::move(*initial));
        require(variable != 0);
        auto& entry = add<OnEventNode>(graph, "Evaluate", std::vector<FuncArgInfo>{{integer, "x"}, {integer, "y"}});
        require(graph.addExport({{1}, graph.nodeId(&entry), 41, {}}));
        Node* polynomial{};
        {
            auto payload = type->create();
            require(payload.has_value());
            payload->get<Polynomial>()->reject = reject;
            auto node = createFlowValueNode(type, std::move(*payload));
            require(node.has_value());
            polynomial = node->get();
            require(graph.addNode(std::move(*node)).valid());
        }
        // Definition and plain payload remain valid after the composition catalog is gone.
        link(graph, *entry.paramPins()[0], *polynomial->inPins()[0]);
        link(graph, *entry.paramPins()[1], *polynomial->inPins()[1]);
        auto& first = add<SetVariableNode>(graph, variable, DataPinInfo{"square", integer});
        auto& second = add<SetVariableNode>(graph, variable, DataPinInfo{"sum", integer});
        auto& end = add<ReturnNode>(graph);
        link(graph, entry.execOutPin(), first.execInPin());
        link(graph, first.execOutPin(), second.execInPin());
        link(graph, second.execOutPin(), end.execInPin());
        link(graph, *polynomial->outPins()[0], first.valueIn());
        link(graph, *polynomial->outPins()[1], second.valueIn());
        return graph;
    }

    void scalarProviderLifetime(const std::filesystem::path& file)
    {
        class Compiler final : public FlowValueCompiler
        {
        public:
            unsigned emitted{};

            const meta::RefType* type(FlowValue value) const noexcept override
            {
                return value <= 1 ? &meta::ref_type_of_v<std::int32_t> : nullptr;
            }

        private:
            FlowForgeResult<FlowValue> emitScalarImpl(
                EScalarInstruction operation,
                std::span<const FlowValue> inputs,
                const meta::RefType& result
            ) noexcept override
            {
                require(operation == EScalarInstruction::ADD_INTEGER && inputs.size() == 2);
                require(result == meta::ref_type_of_v<std::int32_t>);
                ++emitted;
                return FlowValue{1};
            }
        } compiler;

        auto library = std::make_shared<engine::platform::DynamicLibrary>(file);
        require(library->is_loaded());
        const std::weak_ptr observed = library;
        using Entry = void(std::shared_ptr<const FlowNodeType>&, const object::CodeLease&) noexcept;
        auto entry = library->get_symbol<Entry>("makeScalarDefinition");
        require(entry != nullptr);
        std::shared_ptr<const FlowNodeType> definition;
        entry(definition, object::CodeLease::plugin(library));
        std::weak_ptr weak_definition = definition;
        library.reset();
        require(!observed.expired());
        {
            auto payload = definition->create();
            require(payload.has_value());
            auto copy = payload->clone();
            require(copy.has_value());
            auto encoded = definition->encode(*copy);
            require(encoded.has_value());
            auto decoded = definition->decode(*encoded);
            require(decoded.has_value());
            const std::array<FlowValue, 2> inputs{0, 0};
            require(definition->compile(*decoded, inputs, compiler).has_value());
            require(compiler.emitted == 1);
            definition.reset();
            require(!observed.expired()); // Payloads still own their callback and metadata provider.
        }
        require(observed.expired());
        weak_definition.reset(); // The stable host control block also survives provider unload.
        std::puts("PASS DLL builtin scalar definition: callback, clone, codec, payload cleanup and late weak release");
    }
} // namespace

int main(int argc, char** argv)
{
    meta::meta_module_init();
    if (argc >= 2)
    {
        scalarProviderLifetime(std::filesystem::path{argv[1]});
    }
    std::weak_ptr<engine::platform::DynamicLibrary> observed;
    {
        auto definition = registration();
        std::shared_ptr<const FlowNodeType> type;
        if (argc >= 2)
        {
            auto library = std::make_shared<engine::platform::DynamicLibrary>(std::filesystem::path{argv[1]});
            require(library->is_loaded());
            if (argc == 3)
            {
                using Entry = void(std::shared_ptr<const FlowNodeType>&, const object::CodeLease&) noexcept;
                const auto entry = library->get_symbol<Entry>("makeDefinition");
                require(entry != nullptr);
                entry(type, object::CodeLease::plugin(library));
                using Register = void(FlowNodeRegistration&, const object::CodeLease&) noexcept;
                const auto registration = library->get_symbol<Register>("registerPolynomial");
                require(registration != nullptr);
                registration(definition, object::CodeLease::plugin(library));
            }
            else
            {
                using Entry = void(FlowNodeRegistration&, const object::CodeLease&) noexcept;
                const auto entry = library->get_symbol<Entry>("registerPolynomial");
                require(entry != nullptr);
                entry(definition, object::CodeLease::plugin(library));
            }
            observed = library;
        }
        if (!type)
        {
            FlowNodeCatalog catalog;
            require(catalog.add(std::span{&definition, 1}).has_value());
            type = catalog.find(definition.identity.id);
        }
        auto graph = example(false, type);
        auto invalid = example(true, type);
        const asset::AssetId source_id{std::array<std::uint8_t, 16>{1}};
        auto source = captureFlowSource(source_id, "polynomial", graph);
        require(source.has_value());
        auto bytes = encodeFlowSource(*source);
        require(bytes.has_value());
        auto decoded = decodeFlowSource(*bytes);
        require(decoded.has_value() && *decoded == *source);
        {
            FlowNodeCatalog decoding_catalog;
            require(decoding_catalog.add(std::span{&definition, 1}).has_value());
            FlowSourceEnvironment environment;
            environment.nodes = &decoding_catalog;
            auto restored = materializeFlowSource(*decoded, environment);
            require(restored.has_value());
            auto recaptured = captureFlowSource(source_id, "polynomial", *restored);
            require(recaptured.has_value() && *recaptured == *source);
            graph = std::move(*restored);
        }
        type.reset();
        definition = {};
        require(argc < 2 || !observed.expired());
        auto object = compileFlowForgeObject(graph, {.module_name = "registered_polynomial"});
        if (!object)
        {
            std::fprintf(stderr, "%s\n", object.error().message.c_str());
        }
        require(object.has_value());
        auto artifact = linkFlowForgeObject(*object);
        require(artifact.has_value());
        auto module = script::loadNativeModule(artifact->payload(), "registered_polynomial");
        require(module.has_value());
        const auto* function = module->findFunction(script::ScriptSymbolId{41});
        require(function != nullptr);
        require(function->arg_count == 2 && function->invoke != nullptr);
        require(module->stateSize() == sizeof(int));
        int state{};
        lux_script_native_instance_context context{&state, nullptr, 0, 0};
        for (int x = -5; x <= 5; ++x)
        {
            for (int y = -3; y <= 3; ++y)
            {
                std::array<int, 2> values{x, y};
                std::array<lux_script_value_slot, 2> arguments{};
                for (std::size_t i{}; i != arguments.size(); ++i)
                {
                    const auto& type = function->args[i];
                    require(type.size == sizeof(int));
                    arguments[i] = {type.kind, {}, type.size, type.type_id, &values[i]};
                }
                lux_script_call_frame frame{arguments.data(), 2, 0, nullptr, 0, 0, nullptr};
                require(function->invoke(&context, &frame) == 0);
                if (state != x * x + y)
                {
                    std::fprintf(stderr, "x=%d y=%d actual=%d expected=%d\n", x, y, state, x * x + y);
                }
                require(state == x * x + y);
            }
        }
        auto rejected = compileFlowForgeObject(invalid, {.module_name = "rejected_polynomial"});
        require(!rejected && rejected.error().message == "compile failed: polynomial rejected");
        std::puts(
            "PASS registered multi-output polynomial: 77 native invocations, schema draft retained, compile rejected"
        );
    }
    require(observed.expired());
    meta::meta_module_deinit();
}
