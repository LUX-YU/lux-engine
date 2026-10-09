#pragma once

#include <string_view>

#include <lux/engine/material/graph/Types.hpp>
#include <lux/engine/material/graph/visibility.h>

namespace lux::material
{
    enum class EMathOp : std::uint8_t
    {
        // binary (2 operands)
        MUL,
        ADD,
        SUB,
        DIV,
        DOT, ///< -> Float
        MIN,
        MAX,
        POW,
        STEP,
        MOD,
        CROSS, ///< Vec3 x Vec3 -> Vec3
        REFLECT,
        // ternary (currently unsupported by the 2-pin Math node)
        LERP,
        // unary (1 operand; pin 1 ignored)
        SATURATE,
        ONE_MINUS, ///< 1 - x
        ABS,
        SQRT,
        FLOOR,
        FRACT,
        SIN,
        COS,
        NORMALIZE,
        LENGTH ///< -> Float
    };

    // Semantic payload only: the catalog definition provides pins and compilation.
    struct MaterialMath final
    {
        static constexpr std::string_view TypeName{"lux.material.math.v1"};
        EMathOp op{EMathOp::MUL};
        EValueType operand_type{EValueType::FLOAT};
    };

    struct MaterialNodeRegistration;

    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialNodeRegistration materialMathRegistration() noexcept;
} // namespace lux::material
