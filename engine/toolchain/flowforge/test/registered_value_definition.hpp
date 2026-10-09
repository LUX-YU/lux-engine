#pragma once

#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <array>

namespace flow_test
{
    using namespace lux;
    using namespace lux::flowforge;

    struct Polynomial final
    {
        bool reject{};
    };

    inline FlowForgeResult<std::unique_ptr<Polynomial>> clone(const Polynomial& value) noexcept
    {
        return std::make_unique<Polynomial>(value);
    }

    inline FlowNodeRegistration registration(object::CodeLease code = object::CodeLease::builtin())
    {
        FlowNodeRegistration result;
        result.code = std::move(code);
        result.identity = {graph::nodeTypeId("external.polynomial"), "external.polynomial", 1};
        result.payload_type = cxx::typeToken<Polynomial>();
        result.create = [](const object::CodeLease& code) noexcept
        { return FlowNodePayload::make<Polynomial, clone>(code); };
        result.describe_pins = [](const FlowNodePayload&) noexcept -> FlowNodeRegistration::PinResult
        {
            const auto* integer = &meta::ref_type_of_v<int>;
            return std::vector<FlowPinDeclaration>{
                {{11}, "x", graph::EPinDirection::INPUT, integer},
                {{13}, "y", graph::EPinDirection::INPUT, integer},
                {{17}, "square", graph::EPinDirection::OUTPUT, integer},
                {{19}, "sum", graph::EPinDirection::OUTPUT, integer}
            };
        };
        result.validate = [](const FlowNodePayload& payload) noexcept -> FlowForgeResult<void>
        {
            if (payload.get<Polynomial>()->reject)
            {
                return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "polynomial rejected"});
            }
            return {};
        };
        result.compile = [](const FlowNodePayload&, std::span<const FlowValue> inputs, FlowValueCompiler& compiler
                         ) noexcept -> FlowNodeRegistration::ValueResult
        {
            const std::array square_inputs{inputs[0], inputs[0]};
            auto square = compiler.emitScalar(EScalarInstruction::MULTIPLY_INTEGER, square_inputs);
            if (!square)
            {
                return cxx::unexpected(std::move(square.error()));
            }
            const std::array sum_inputs{*square, inputs[1]};
            auto sum = compiler.emitScalar(EScalarInstruction::ADD_INTEGER, sum_inputs);
            if (!sum)
            {
                return cxx::unexpected(std::move(sum.error()));
            }
            return std::vector<FlowValue>{*square, *sum};
        };
        return result;
    }

} // namespace flow_test
