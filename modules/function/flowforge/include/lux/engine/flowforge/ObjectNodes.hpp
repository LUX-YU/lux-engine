#pragma once

#include <lux/engine/flowforge/FlowNodeCatalog.hpp>

namespace lux::flowforge
{
    // Metadata is an immutable borrowed environment, retained by the graph's source owner.
    struct LUX_ENGINE_FLOWFORGE_PUBLIC GetObjectPayload final
    {
        const meta::RefType* type{};

        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC SetObjectPayload final
    {
        const meta::RefType* type{};

        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    // A field read is evaluated at each use; it must never be cached as a pure scalar expression.
    struct LUX_ENGINE_FLOWFORGE_PUBLIC GetFieldPayload final
    {
        const meta::RefClass* owner{};
        const meta::RefField* field{};

        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC SetFieldPayload final
    {
        const meta::RefClass* owner{};
        const meta::RefField* field{};

        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    // A variable read also observes current instance state at each use. Identity survives renaming.
    struct LUX_ENGINE_FLOWFORGE_PUBLIC GetVariablePayload final
    {
        std::uint64_t variable{};
        const meta::RefType* type{};

        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
        [[nodiscard]] FlowForgeResult<void> validateReferences(FlowReferenceView) const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC SetVariablePayload final
    {
        std::uint64_t variable{};
        const meta::RefType* type{};

        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
        [[nodiscard]] FlowForgeResult<void> validateReferences(FlowReferenceView) const noexcept;
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC std::vector<FlowNodeRegistration>
    objectNodeRegistrations(object::CodeLease code = object::CodeLease::builtin()) noexcept;
} // namespace lux::flowforge
