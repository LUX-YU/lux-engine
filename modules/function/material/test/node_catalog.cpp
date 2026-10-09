#include <lux/engine/material/MaterialNodeCatalog.hpp>

#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::material;

    void require(bool condition, std::source_location location = std::source_location::current()) noexcept
    {
        if (!condition)
        {
            std::fprintf(stderr, "require failed at line %u\n", location.line());
            std::abort();
        }
    }

    struct State final
    {
        int constructed{};
        int destroyed{};
        bool released{};
    };

    struct Payload final
    {
        explicit Payload(State* state = nullptr) noexcept : state(state)
        {
            if (state)
            {
                ++state->constructed;
            }
        }

        ~Payload()
        {
            if (state)
            {
                require(!state->released);
                ++state->destroyed;
            }
        }

        State* state{};
        unsigned outputs{2};
        bool reject_clone{};
        bool invalid_pins{};
        bool invalid_output{};
        bool invalid_output_use{};
        EMaterialInputUse input_use{EMaterialInputUse::VALUE};
    };

    MaterialNodeResult<std::unique_ptr<Payload>> clonePayload(const Payload& source) noexcept
    {
        if (source.reject_clone)
        {
            return cxx::unexpected(MaterialCompileFailure{EMaterialCompileError::INVALID_RESULT, "clone rejected"});
        }
        if (source.outputs == 9)
        {
            return std::unique_ptr<Payload>{};
        }
        auto copy = std::make_unique<Payload>(source.state);
        copy->outputs = source.outputs;
        copy->invalid_pins = source.invalid_pins;
        copy->invalid_output = source.invalid_output;
        copy->invalid_output_use = source.invalid_output_use;
        copy->input_use = source.input_use;
        return copy;
    }

    MaterialNodeRegistration registration(std::string name = "test.material.multiple.v1") noexcept
    {
        MaterialNodeRegistration result;
        result.identity = {graph::nodeTypeId(name), std::move(name), 1};
        result.payload_type = cxx::typeToken<Payload>();
        result.create = [](const object::CodeLease& code) noexcept
        { return MaterialNodePayload::make<Payload, &clonePayload>(code); };
        result.validate = [](const MaterialNodePayload& payload) noexcept -> MaterialNodeResult<void>
        {
            if (payload.get<Payload>()->outputs == 0)
            {
                return cxx::unexpected(MaterialCompileFailure{EMaterialCompileError::INVALID_GRAPH, "no outputs"});
            }
            return {};
        };
        result.describe_pins = [](const MaterialNodePayload& payload
                               ) noexcept -> MaterialNodeResult<std::vector<MaterialPinDeclaration>>
        {
            const auto& value = *payload.get<Payload>();
            std::vector<MaterialPinDeclaration> pins;
            pins.push_back({graph::PinSemanticId{1}, "input", graph::EPinDirection::INPUT});
            pins.back().input_use = value.input_use;
            for (unsigned i = 0; i != value.outputs; ++i)
            {
                pins.push_back(
                    {graph::PinSemanticId{value.invalid_pins ? 1U : i + 2U},
                     "output " + std::to_string(i),
                     graph::EPinDirection::OUTPUT}
                );
            }
            if (value.invalid_output_use)
            {
                pins.back().input_use = EMaterialInputUse::UNUSED;
            }
            return pins;
        };
        result.compile = [](const MaterialNodePayload& payload,
                            std::span<const std::uint32_t> inputs,
                            shadergen::ShaderIR& ir) noexcept -> MaterialNodeResult<std::vector<std::uint32_t>>
        {
            const auto& value = *payload.get<Payload>();
            std::vector<std::uint32_t> outputs;
            for (unsigned i = 0; i != value.outputs; ++i)
            {
                const auto index = static_cast<std::uint32_t>(ir.values.size());
                shadergen::ShaderIRValue expression{shadergen::EOp::ADD, shadergen::EValueType::FLOAT};
                if (inputs[0] == shadergen::kNoValue)
                {
                    expression.op = shadergen::EOp::CONSTANT;
                }
                else
                {
                    expression.operands[0] = inputs[0];
                    expression.operands[1] = inputs[0];
                }
                ir.values.push_back(expression);
                outputs.push_back(value.invalid_output ? shadergen::kNoValue : index);
            }
            return outputs;
        };
        return result;
    }
} // namespace

int main()
{
    static_assert(!std::is_copy_constructible_v<MaterialNodePayload>);
    static_assert(std::is_nothrow_move_constructible_v<MaterialNodePayload>);
    static_assert(std::is_nothrow_move_assignable_v<MaterialNodePayload>);
    static_assert(!std::is_move_constructible_v<MaterialNodeCatalog>);
    using ThrowingCreate = MaterialNodeResult<MaterialNodePayload> (*)(const object::CodeLease&);
    static_assert(!std::is_convertible_v<ThrowingCreate, MaterialNodeRegistration::Create>);
    std::shared_ptr<const MaterialNodeType> type;
    {
        MaterialNodeCatalog catalog;
        auto input = registration();
        std::string transient_type_name(input.payload_type.name());
        input.payload_type = cxx::TypeToken{input.payload_type.hash(), transient_type_name};
        require(catalog.add({&input, 1}).has_value());
        type = catalog.find(input.identity.id);
        require(bool(type));
        input.identity.canonical_name = "overwritten";
        transient_type_name.assign(transient_type_name.size(), '?');
        require(type->identity().canonical_name == "test.material.multiple.v1");
        require(type->create().has_value());
        auto duplicate = registration();
        auto rejected = catalog.add({&duplicate, 1});
        require(!rejected && rejected.error() == EMaterialNodeCatalogError::DUPLICATE_TYPE);
        auto collision = registration("test.material.collision.v1");
        collision.identity.id = duplicate.identity.id;
        rejected = catalog.add({&collision, 1});
        require(!rejected && rejected.error() == EMaterialNodeCatalogError::HASH_COLLISION);
        MaterialNodeRegistration batch[]{registration("test.material.unpublished.v1"), duplicate};
        require(!catalog.add(batch));
        require(!catalog.find(batch[0].identity.id));
        require(catalog.find(duplicate.identity.id) == type);
        auto invalid = registration("test.material.invalid.v1");
        invalid.compile = nullptr;
        rejected = catalog.add({&invalid, 1});
        require(!rejected && rejected.error() == EMaterialNodeCatalogError::INVALID_REGISTRATION);
        invalid = registration("test.material.mismatched_hash.v1");
        invalid.identity.id = graph::NodeTypeId{1};
        require(!catalog.add({&invalid, 1}));
        require(!catalog.find(invalid.identity.id));
        invalid = registration("test.material.invalid_version.v1");
        invalid.identity.version = 0;
        require(!catalog.add({&invalid, 1}));
        invalid = registration("invalid name");
        require(!catalog.add({&invalid, 1}));
        auto empty_factory = registration("test.material.empty_factory.v1");
        empty_factory.create = [](const object::CodeLease&) noexcept -> MaterialNodeResult<MaterialNodePayload>
        { return MaterialNodePayload{}; };
        require(catalog.add({&empty_factory, 1}).has_value());
        require(!catalog.find(empty_factory.identity.id)->create());
    }
    auto payload = type->create();
    require(payload.has_value());
    payload->get<Payload>()->outputs = 0;
    require(!type->validate(*payload));
    payload->get<Payload>()->outputs = 2;
    require(type->describePins(*payload)->size() == 3);
    payload->get<Payload>()->outputs = 3;
    require(type->describePins(*payload)->size() == 4);
    shadergen::ShaderIR candidate;
    candidate.values.push_back({shadergen::EOp::CONSTANT, shadergen::EValueType::FLOAT});
    std::uint32_t inputs[]{0};
    auto outputs = type->compile(*payload, inputs, candidate);
    require(outputs.has_value() && outputs->size() == 3);
    require(candidate.values.size() == 4 && outputs->back() == 3);
    require(candidate.values[3].operands[0] == 0);
    std::uint32_t bad_input[]{shadergen::kNoValue};
    const auto prior = shadergen::computeFingerprint(candidate);
    require(!type->compile(*payload, bad_input, candidate));
    require(shadergen::computeFingerprint(candidate) == prior);
    payload->get<Payload>()->input_use = EMaterialInputUse::CONNECTED_VALUE;
    require(type->compile(*payload, bad_input, candidate).has_value());
    require(type->compile(*payload, inputs, candidate).has_value());
    payload->get<Payload>()->input_use = EMaterialInputUse::UNUSED;
    require(type->compile(*payload, bad_input, candidate).has_value());
    require(!type->compile(*payload, inputs, candidate));
    payload->get<Payload>()->input_use = static_cast<EMaterialInputUse>(255);
    require(!type->describePins(*payload));
    payload->get<Payload>()->input_use = EMaterialInputUse::VALUE;
    payload->get<Payload>()->invalid_output_use = true;
    require(!type->describePins(*payload));
    payload->get<Payload>()->invalid_output_use = false;
    payload->get<Payload>()->invalid_pins = true;
    require(!type->describePins(*payload));
    payload->get<Payload>()->invalid_pins = false;
    payload->get<Payload>()->invalid_output = true;
    require(!type->compile(*payload, inputs, candidate));
    payload->get<Payload>()->reject_clone = true;
    auto rejected_clone = payload->clone();
    require(!rejected_clone && rejected_clone.error().message == "clone rejected");
    require(payload->get<Payload>()->outputs == 3);
    payload->get<Payload>()->reject_clone = false;
    auto copy = payload->clone();
    require(copy.has_value() && copy->get<Payload>() != payload->get<Payload>());
    require(copy->get<Payload>()->outputs == 3);
    payload->get<Payload>()->outputs = 9;
    require(!payload->clone());
    require(payload->get<Payload>()->outputs == 9);

    State old;
    State next;
    const auto make = [](State& state) noexcept
    {
        auto owner = std::shared_ptr<const void>(
            &state,
            [](const void* value) noexcept
            {
                auto& state = *const_cast<State*>(static_cast<const State*>(value));
                require(state.constructed == state.destroyed);
                state.released = true;
            }
        );
        return MaterialNodePayload::make<Payload, &clonePayload>(object::CodeLease::plugin(std::move(owner)), &state);
    };
    auto first = make(old);
    auto second = make(next);
    require(first.has_value() && second.has_value());
    *first = std::move(*second);
    require(old.released && old.destroyed == 1);
    require(!next.released);
    *first = MaterialNodePayload{};
    require(next.released && next.destroyed == 1);
    require(!second->clone());
    State not_constructed;
    auto invalid_code =
        MaterialNodePayload::make<Payload, &clonePayload>(object::CodeLease::plugin({}), &not_constructed);
    require(!invalid_code && not_constructed.constructed == 0);
    MaterialNodePayload empty;
    require(!type->validate(empty));
    std::puts("PASS: owned Material definitions, dynamic pins/SSA, rejected batches, clone and lease order");
}
