#pragma once

#include <lux/engine/flowforge/FlowNodeCatalog.hpp>

namespace lux::flowforge
{
    struct LUX_ENGINE_FLOWFORGE_PUBLIC StartPayload final
    {
        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC BranchPayload final
    {
        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC SequencePayload final
    {
        std::size_t additional_outputs{};

        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    // Index values are int32; Last Index is exclusive: [first, last).
    struct LUX_ENGINE_FLOWFORGE_PUBLIC ForLoopPayload final
    {
        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC WhileLoopPayload final
    {
        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC ReturnPayload final
    {
        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC BreakPayload final
    {
        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC std::vector<FlowNodeRegistration> controlNodeRegistrations(
        object::CodeLease code = object::CodeLease::builtin()
    ) noexcept;
} // namespace lux::flowforge
