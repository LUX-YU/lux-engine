#pragma once
#include <cmath>
#include <cstdint>

namespace lux::input
{
    // ------------------------------------------------------------------ //
    //  Lightweight math helpers (engine-internal, avoid Eigen dependency)  //
    // ------------------------------------------------------------------ //

    struct Float2
    {
        float x{0.0f};
        float y{0.0f};
    };

    struct Float3
    {
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
    };

    // ------------------------------------------------------------------ //
    //  Value type tag                                                      //
    // ------------------------------------------------------------------ //

    enum class EInputValueType : uint8_t
    {
        BOOL,
        AXIS_1D,
        AXIS_2D,
        AXIS_3D,
    };

    // ------------------------------------------------------------------ //
    //  InputValue — a tagged union that can carry Bool / 1D / 2D / 3D     //
    // ------------------------------------------------------------------ //

    struct InputValue
    {
        EInputValueType type{EInputValueType::BOOL};
        Float3 v{};

        // ── Factories ────────────────────────────────────────────────── //

        static InputValue makeBool(bool b)
        {
            InputValue out;
            out.type = EInputValueType::BOOL;
            out.v.x = b ? 1.0f : 0.0f;
            return out;
        }

        static InputValue makeAxis1D(float x)
        {
            InputValue out;
            out.type = EInputValueType::AXIS_1D;
            out.v.x = x;
            return out;
        }

        static InputValue makeAxis2D(float x, float y)
        {
            InputValue out;
            out.type = EInputValueType::AXIS_2D;
            out.v.x = x;
            out.v.y = y;
            return out;
        }

        static InputValue makeAxis3D(float x, float y, float z)
        {
            InputValue out;
            out.type = EInputValueType::AXIS_3D;
            out.v.x = x;
            out.v.y = y;
            out.v.z = z;
            return out;
        }

        static InputValue zeroOf(EInputValueType t)
        {
            switch (t)
            {
            case EInputValueType::BOOL:
                return makeBool(false);
            case EInputValueType::AXIS_1D:
                return makeAxis1D(0.0f);
            case EInputValueType::AXIS_2D:
                return makeAxis2D(0.0f, 0.0f);
            case EInputValueType::AXIS_3D:
                return makeAxis3D(0.0f, 0.0f, 0.0f);
            }
            return makeBool(false);
        }

        // ── Accessors ───────────────────────────────────────────────── //

        [[nodiscard]] bool asBool() const noexcept
        {
            return std::fabs(v.x) > 1e-6f;
        }
        [[nodiscard]] float as1D() const noexcept
        {
            return v.x;
        }
        [[nodiscard]] Float2 as2D() const noexcept
        {
            return {v.x, v.y};
        }
        [[nodiscard]] Float3 as3D() const noexcept
        {
            return v;
        }

        [[nodiscard]] bool nearlyZero(float eps = 1e-6f) const noexcept
        {
            switch (type)
            {
            case EInputValueType::BOOL:
            case EInputValueType::AXIS_1D:
                return std::fabs(v.x) <= eps;
            case EInputValueType::AXIS_2D:
                return std::fabs(v.x) <= eps && std::fabs(v.y) <= eps;
            case EInputValueType::AXIS_3D:
                return std::fabs(v.x) <= eps && std::fabs(v.y) <= eps && std::fabs(v.z) <= eps;
            }
            return true;
        }
    };

    // ── Free-function accumulator ───────────────────────────────────── //

    /// Typed accumulation — the accumulator is initialized to the correct
    /// target type so that a Bool-zero default doesn't discard axis values.
    /// The accumulator is initialized to the correct target type so that a
    /// Bool-zero default doesn't discard axis contributions.
    inline InputValue accumulateValue(const InputValue& a, const InputValue& b, EInputValueType target_type)
    {
        // If accumulator is still the default-constructed zero, initialize it
        // to the target type's zero before accumulating.
        InputValue lhs = a;
        if (a.nearlyZero() && a.type != target_type)
            lhs = InputValue::zeroOf(target_type);

        // Type mismatch: keep accumulated value.
        if (lhs.type != b.type)
            return lhs;

        InputValue out = lhs;
        switch (lhs.type)
        {
        case EInputValueType::BOOL:
            out.v.x = (lhs.asBool() || b.asBool()) ? 1.0f : 0.0f;
            break;
        case EInputValueType::AXIS_1D:
            out.v.x = lhs.v.x + b.v.x;
            break;
        case EInputValueType::AXIS_2D:
            out.v.x = lhs.v.x + b.v.x;
            out.v.y = lhs.v.y + b.v.y;
            break;
        case EInputValueType::AXIS_3D:
            out.v.x = lhs.v.x + b.v.x;
            out.v.y = lhs.v.y + b.v.y;
            out.v.z = lhs.v.z + b.v.z;
            break;
        }
        return out;
    }

} // namespace lux::input
