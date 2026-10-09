#pragma once

#include <lux/engine/flowforge/FlowForgeFailure.hpp>
#include <lux/engine/flowforge/visibility.h>

#include <cstdint>
#include <span>

namespace lux::meta
{
    struct RefType;
}

namespace lux::flowforge
{
    // Compiler-local instructions, never graph type identities or serialized values.
    // The domain selects signedness and ordered floating comparison semantics;
    // the backend only translates the selected instruction to its own IR.
    enum class EScalarInstruction : std::uint8_t
    {
        ADD_INTEGER,
        ADD_FLOAT,
        SUBTRACT_INTEGER,
        SUBTRACT_FLOAT,
        MULTIPLY_INTEGER,
        MULTIPLY_FLOAT,
        DIVIDE_SIGNED,
        DIVIDE_UNSIGNED,
        DIVIDE_FLOAT,
        REMAINDER_SIGNED,
        REMAINDER_UNSIGNED,
        REMAINDER_FLOAT,
        AND,
        OR,
        EQUAL_INTEGER,
        NOT_EQUAL_INTEGER,
        LESS_SIGNED,
        LESS_UNSIGNED,
        LESS_EQUAL_SIGNED,
        LESS_EQUAL_UNSIGNED,
        GREATER_SIGNED,
        GREATER_UNSIGNED,
        GREATER_EQUAL_SIGNED,
        GREATER_EQUAL_UNSIGNED,
        EQUAL_ORDERED_FLOAT,
        NOT_EQUAL_ORDERED_FLOAT,
        LESS_ORDERED_FLOAT,
        LESS_EQUAL_ORDERED_FLOAT,
        GREATER_ORDERED_FLOAT,
        GREATER_EQUAL_ORDERED_FLOAT,
        NEGATE_INTEGER,
        NEGATE_FLOAT,
        NOT_BOOLEAN
    };

    using FlowValue = std::uint32_t;
    inline constexpr FlowValue kInvalidFlowValue = ~FlowValue{0};

    // Synchronous, non-owning code-generation boundary. Values are local to this invocation;
    // callbacks must not retain the compiler or value handles. No authoring topology lives here.
    class LUX_ENGINE_FLOWFORGE_PUBLIC FlowValueCompiler
    {
    public:
        virtual ~FlowValueCompiler() = default;
        [[nodiscard]] virtual const meta::RefType* type(FlowValue) const noexcept = 0;
        [[nodiscard]] FlowForgeResult<FlowValue> emitScalar(EScalarInstruction, std::span<const FlowValue>) noexcept;

    protected:
        [[nodiscard]] virtual FlowForgeResult<FlowValue> emitScalarImpl(
            EScalarInstruction,
            std::span<const FlowValue>,
            const meta::RefType& result_type
        ) noexcept = 0;
    };
} // namespace lux::flowforge
