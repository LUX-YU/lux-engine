#pragma once
#include "InputValue.hpp"
#include <vector>
#include <algorithm>

namespace lux::input
{
    // ------------------------------------------------------------------ //
    //  Modifier kind (enum-configured, no virtual dispatch)               //
    // ------------------------------------------------------------------ //

    enum class EModifierKind : uint8_t
    {
        NONE,
        SCALE,         ///< Multiply each axis by (x, y, z)
        NEGATE_X,      ///< Flip X
        NEGATE_Y,      ///< Flip Y
        NEGATE_Z,      ///< Flip Z
        DEAD_ZONE,     ///< Zero output if magnitude < threshold (x)
        NORMALIZE_2D,  ///< Normalize the XY vector to unit length
        CLAMP_1D,      ///< Clamp AXIS_1D value to [x, y]
    };

    struct ModifierSpec
    {
        EModifierKind kind{EModifierKind::NONE};
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
    };

    // ------------------------------------------------------------------ //
    //  Apply a single modifier                                            //
    // ------------------------------------------------------------------ //

    inline InputValue applyModifier(const InputValue& in, const ModifierSpec& mod)
    {
        InputValue out = in;

        switch (mod.kind)
        {
        case EModifierKind::NONE:
            return out;

        case EModifierKind::SCALE:
            out.v.x *= mod.x;
            out.v.y *= mod.y;
            out.v.z *= mod.z;
            return out;

        case EModifierKind::NEGATE_X:
            out.v.x = -out.v.x;
            return out;

        case EModifierKind::NEGATE_Y:
            out.v.y = -out.v.y;
            return out;

        case EModifierKind::NEGATE_Z:
            out.v.z = -out.v.z;
            return out;

        case EModifierKind::DEAD_ZONE:
            if (in.type == EInputValueType::AXIS_1D)
            {
                if (std::fabs(out.v.x) < mod.x) out.v.x = 0.0f;
            }
            else if (in.type == EInputValueType::AXIS_2D)
            {
                const float len = std::sqrt(out.v.x * out.v.x + out.v.y * out.v.y);
                if (len < mod.x)
                {
                    out.v.x = 0.0f;
                    out.v.y = 0.0f;
                }
            }
            return out;

        case EModifierKind::NORMALIZE_2D:
            if (in.type == EInputValueType::AXIS_2D)
            {
                const float len = std::sqrt(out.v.x * out.v.x + out.v.y * out.v.y);
                if (len > 1e-6f)
                {
                    out.v.x /= len;
                    out.v.y /= len;
                }
            }
            return out;

        case EModifierKind::CLAMP_1D:
            if (in.type == EInputValueType::AXIS_1D)
                out.v.x = std::clamp(out.v.x, mod.x, mod.y);
            return out;
        }

        return out;
    }

    // ------------------------------------------------------------------ //
    //  Apply a chain of modifiers                                         //
    // ------------------------------------------------------------------ //

    inline InputValue applyModifiers(const InputValue& in,
                                     const std::vector<ModifierSpec>& mods)
    {
        InputValue out = in;
        for (const auto& m : mods)
            out = applyModifier(out, m);
        return out;
    }

} // namespace lux::input
