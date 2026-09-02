#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace lux::ui
{
    struct EditResult final
    {
        bool changed{};
        bool began{};
        bool committed{};
        bool cancelled{};

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return changed; }
    };

    enum class EScalarEditMode : std::uint8_t
    {
        INPUT,
        DRAG,
        SLIDER,
    };

    template<class Value>
    struct ScalarEditSpec final
    {
        EScalarEditMode mode{EScalarEditMode::DRAG};
        float speed{0.1F};
        std::optional<Value> minimum;
        std::optional<Value> maximum;
        std::optional<Value> step;
        std::string_view format;
    };

    struct InputTextSpec final
    {
        std::string_view hint;
        bool read_only{};
    };

    struct ComboOption final
    {
        std::int64_t value{};
        std::string_view label;
    };
} // namespace lux::ui
