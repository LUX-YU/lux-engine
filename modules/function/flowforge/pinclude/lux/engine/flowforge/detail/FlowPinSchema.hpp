#pragma once

#include <lux/engine/flowforge/FlowNodeCatalog.hpp>

namespace lux::flowforge::detail
{
    inline FlowForgeResult<meta::RuntimeObject> initialZeroValue(
        const FlowNodePayload&,
        const meta::RefType& type
    ) noexcept
    {
        auto result = meta::RuntimeObject::defaultOf(type);
        // Preserve original input default initialization: unsupported default types remain link-only.
        return result ? std::move(*result) : meta::RuntimeObject{};
    }

    // Frozen source semantics: ordinal is within all inputs or all outputs, including execution pins.
    [[nodiscard]] constexpr graph::PinSemanticId pinSemantic(
        EFlowPinRole role,
        graph::EPinDirection direction,
        std::size_t ordinal
    ) noexcept
    {
        const auto category = role == EFlowPinRole::EXECUTION ? 2U : 4U;
        const auto side = direction == graph::EPinDirection::OUTPUT ? 1U : 0U;
        return graph::PinSemanticId{(static_cast<std::uint64_t>(category + side) << 56U) | (ordinal + 1U)};
    }

    // Function and script signatures place data after their leading execution pin.
    [[nodiscard]] inline std::vector<graph::PinSemanticId> parameterSemantics(
        graph::EPinDirection direction,
        std::size_t count
    ) noexcept
    {
        std::vector<graph::PinSemanticId> result;
        result.reserve(count);
        for (std::size_t i = 0; i != count; ++i)
        {
            result.push_back(pinSemantic(EFlowPinRole::DATA, direction, i + 1));
        }
        return result;
    }

    inline void appendExecutionPin(
        std::vector<FlowPinDeclaration>& pins,
        graph::EPinDirection direction,
        std::string name,
        std::size_t ordinal = 0
    ) noexcept
    {
        pins.push_back(
            {pinSemantic(EFlowPinRole::EXECUTION, direction, ordinal),
             std::move(name),
             direction,
             nullptr,
             false,
             EFlowPinRole::EXECUTION}
        );
    }

    inline void appendDataPin(
        std::vector<FlowPinDeclaration>& pins,
        graph::EPinDirection direction,
        std::size_t ordinal,
        std::string name,
        const meta::RefType* type,
        bool allow_default = false
    ) noexcept
    {
        pins.push_back(
            {pinSemantic(EFlowPinRole::DATA, direction, ordinal), std::move(name), direction, type, allow_default}
        );
        if (direction == graph::EPinDirection::INPUT)
        {
            pins.back().initial_value = initialZeroValue;
        }
    }
} // namespace lux::flowforge::detail
