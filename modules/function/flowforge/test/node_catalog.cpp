#include <exception>
#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/flowforge/ScalarNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool condition, std::source_location location = std::source_location::current()) noexcept
    {
        if (!condition)
        {
            std::fprintf(stderr, "catalog contract failed at %u\n", location.line());
            std::abort();
        }
    }

    struct Payload final
    {
        int mode{};
    };

    FlowForgeResult<std::unique_ptr<Payload>> clone(const Payload& source) noexcept
    {
        return std::make_unique<Payload>(source);
    }

    FlowNodeRegistration descriptor(std::string name)
    {
        FlowNodeRegistration result;
        result.identity = {graph::nodeTypeId(name), std::move(name), 1};
        result.payload_type = cxx::typeToken<Payload>();
        result.create = [](const object::CodeLease& code) noexcept
        { return FlowNodePayload::make<Payload, clone>(code); };
        result.describe_pins = [](const FlowNodePayload& payload) noexcept -> FlowNodeRegistration::PinResult
        {
            const auto output = payload.get<Payload>()->mode == 3 ? 1U : 2U;
            return std::vector<FlowPinDeclaration>{
                {{1}, "input", graph::EPinDirection::INPUT, &meta::ref_type_of_v<int>},
                {{output}, "output", graph::EPinDirection::OUTPUT, &meta::ref_type_of_v<int>}
            };
        };
        result.validate = [](const FlowNodePayload& payload) noexcept -> FlowForgeResult<void>
        {
            if (payload.get<Payload>()->mode == 1)
            {
                return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "draft not compilable"});
            }
            return {};
        };
        result.compile = [](const FlowNodePayload& payload,
                            std::span<const FlowValue> values,
                            FlowValueCompiler&) noexcept -> FlowNodeRegistration::ValueResult
        {
            if (payload.get<Payload>()->mode == 2)
            {
                return std::vector<FlowValue>{kInvalidFlowValue};
            }
            return std::vector<FlowValue>{values.front()};
        };
        result.encode = [](const FlowNodePayload& payload) noexcept -> FlowForgeResult<std::string>
        { return std::to_string(payload.get<Payload>()->mode); };
        result.decode = [](std::string_view bytes,
                           const object::CodeLease& code) noexcept -> FlowForgeResult<FlowNodePayload>
        {
            const bool is_valid = bytes.size() == 1 && bytes.front() >= '0' && bytes.front() <= '3';
            if (!is_valid)
            {
                return cxx::unexpected(FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "invalid payload"});
            }
            return FlowNodePayload::make<Payload, clone>(code, Payload{bytes.front() - '0'});
        };
        return result;
    }

    class RecordingCompiler final : public FlowValueCompiler
    {
    public:
        std::vector<const meta::RefType*> values{&meta::ref_type_of_v<int>, &meta::ref_type_of_v<float>};
        std::size_t emitted{};

        const meta::RefType* type(FlowValue value) const noexcept override
        {
            return value < values.size() ? values[value] : nullptr;
        }

        FlowForgeResult<FlowValue> readVariable(std::uint64_t) noexcept override
        {
            require(false); // This fixture exclusively exercises scalar callbacks.
            std::terminate();
        }

        FlowForgeResult<FlowValue> readField(const meta::RefField&, FlowValue) noexcept override
        {
            require(false);
            std::terminate();
        }

    private:
        FlowForgeResult<FlowValue> emitScalarImpl(
            EScalarInstruction,
            std::span<const FlowValue>,
            const meta::RefType& type
        ) noexcept override
        {
            ++emitted;
            values.push_back(&type);
            return static_cast<FlowValue>(values.size() - 1);
        }
    };

    void scalarDefinitions()
    {
        FlowNodeCatalog catalog;
        require(catalog.add(scalarNodeRegistrations()).has_value());
        require(scalarNodeRegistrations().size() == 15);
        for (const auto& registration : scalarNodeRegistrations())
        {
            const auto type = catalog.find(registration.identity.id);
            require(type && type->identity().canonical_name == registration.identity.canonical_name);
            require(!catalog.add(std::span{&registration, 1}));
            auto payload = type->create();
            require(payload.has_value());
            const auto* operand = payload->get<ScalarNodePayload>()->operand_type;
            auto clone = payload->clone();
            require(clone.has_value() && clone->get<ScalarNodePayload>() != payload->get<ScalarNodePayload>());
            require(clone->get<ScalarNodePayload>()->operand_type == operand);
            auto encoded = type->encode(*payload);
            require(encoded.has_value() && *encoded == operand->name);
            auto decoded = type->decode(*encoded);
            require(decoded.has_value() && decoded->get<ScalarNodePayload>()->operand_type == operand);
            require(!type->decode("not a reflected scalar"));
            auto pins = type->describePins(*payload);
            require(pins.has_value());
            RecordingCompiler compiler;
            compiler.values = {operand};
            std::vector<FlowValue> inputs(pins->size() - 1, 0);
            auto compiled = type->compile(*payload, inputs, compiler);
            require(compiled.has_value() && compiled->size() == 1 && compiler.emitted == 1);
            require(*compiler.type(compiled->front()) == *pins->back().type);
            auto node = createFlowNode(type, std::move(*payload));
            require(node.has_value());
            require(node->definition == type);
            FlowGraph graph;
            const auto id = graph.addNode(std::move(*node));
            require(id.has_value());
            for (const auto& declaration : *pins)
            {
                if (declaration.direction == graph::EPinDirection::INPUT)
                {
                    const auto* input = graph.pin(graph.pinId(*id, declaration.semantic));
                    require(input && input->allow_default && input->default_value.isValid());
                }
            }
            require(graph.topology().findNode(*id)->type == registration.identity.id);
            const asset::AssetId asset_id{std::array<std::uint8_t, 16>{1}};
            auto source = captureFlowSource(asset_id, "scalar", graph);
            require(source.has_value());
            require(std::get<FlowSourceType>(source->nodes.front().parameters).name == *encoded);
            auto restored = materializeFlowSource(*source);
            require(restored.has_value());
            require(*captureFlowSource(asset_id, "scalar", *restored) == *source);
        }
        std::puts(
            "PASS 15 builtin scalar definitions: same catalog, clone, codec, primitive compile and graph roundtrip"
        );
    }
} // namespace

int main()
{
    meta::meta_module_init();
    scalarDefinitions();
    {
        FlowNodeCatalog catalog;
        auto first = descriptor("test.identity");
        auto second = descriptor("test.second");
        auto invalid = second;
        invalid.compile = nullptr;
        const std::array batch{first, invalid};
        require(!catalog.add(batch));
        require(!catalog.find(first.identity.id));
        require(catalog.add(std::span{&first, 1}).has_value());
        require(!catalog.add(std::span{&first, 1}));
        auto collision = descriptor("test.collision");
        collision.identity.id = first.identity.id;
        const auto collided = catalog.add(std::span{&collision, 1});
        require(!collided && collided.error() == EFlowNodeCatalogError::HASH_COLLISION);
        require(catalog.add(std::span{&second, 1}).has_value());
        const auto type = catalog.find(first.identity.id);
        auto payload = type->create();
        require(payload.has_value());
        RecordingCompiler compiler;
        const std::array<FlowValue, 1> inputs{0};
        require(type->compile(*payload, inputs, compiler).has_value());
        const std::array<FlowValue, 1> wrong_type{1};
        require(!type->compile(*payload, wrong_type, compiler));
        require(!type->compile(*payload, {}, compiler));
        payload->get<Payload>()->mode = 1;
        require(type->describePins(*payload).has_value());
        require(!type->compile(*payload, inputs, compiler));
        auto encoded = type->encode(*payload);
        require(encoded.has_value() && *encoded == "1");
        auto decoded = type->decode(*encoded);
        require(decoded.has_value() && decoded->get<Payload>()->mode == 1);
        require(!type->decode("3"));
        require(!type->decode("bad"));
        auto foreign = FlowNodePayload::make<Payload, clone>(object::CodeLease::plugin(std::make_shared<int>(1)));
        require(foreign.has_value() && !type->describePins(*foreign));
        payload->get<Payload>()->mode = 2;
        require(!type->compile(*payload, inputs, compiler));
        payload->get<Payload>()->mode = 3;
        require(!type->describePins(*payload));
        payload->get<Payload>()->mode = 0;
        auto node = createFlowNode(type, std::move(*payload));
        require(node.has_value());
        FlowGraph graph;
        const auto id = graph.addNode(std::move(*node));
        require(id.has_value());
        require(graph.topology().findNode(*id)->type == first.identity.id);
        const std::array<FlowValue, 2> integers{0, 0};
        const std::array<FlowValue, 2> mixed{0, 1};
        const std::array<FlowValue, 2> unknown{0, kInvalidFlowValue};
        require(!compiler.emitScalar(EScalarInstruction::ADD_INTEGER, mixed));
        require(!compiler.emitScalar(EScalarInstruction::ADD_INTEGER, unknown));
        require(!compiler.emitScalar(EScalarInstruction::ADD_INTEGER, inputs));
        require(!compiler.emitScalar(EScalarInstruction::ADD_FLOAT, integers));
        require(!compiler.emitScalar(static_cast<EScalarInstruction>(255), integers));
        require(compiler.emitted == 0);
        auto comparison = compiler.emitScalar(EScalarInstruction::EQUAL_INTEGER, integers);
        require(comparison.has_value());
        require(*compiler.type(*comparison) == meta::ref_type_of_v<bool>);
        require(compiler.emitted == 1);
    }
    meta::meta_module_deinit();
    std::puts("PASS immutable Flow definitions, atomic admission, draft schema, output validation, scalar boundary");
}
