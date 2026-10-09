#pragma once

#include <lux/engine/flowforge/FlowNodeCatalog.hpp>

namespace lux::flowforge
{
    class NativeCallDefinition;

    struct FuncArgInfo final
    {
        // The immutable metadata environment outlives the graph and its snapshots.
        const meta::RefType* type{};
        std::string name;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC FunctionPayload final
    {
        std::vector<FuncArgInfo> arguments;
        std::vector<FuncArgInfo> results;

        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC FunctionReturnPayload final
    {
        graph::NodeId definition;
        std::vector<FuncArgInfo> results;

        [[nodiscard]] bool matchesSignature(const FunctionPayload&) const noexcept;
        [[nodiscard]] FlowForgeResult<void> validateReferences(FlowReferenceView) const noexcept;
        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC FunctionCallPayload final
    {
        graph::NodeId callee;
        std::vector<FuncArgInfo> arguments;
        std::vector<FuncArgInfo> results;

        [[nodiscard]] bool matchesSignature(const FunctionPayload&) const noexcept;
        [[nodiscard]] FlowForgeResult<void> validateReferences(FlowReferenceView) const noexcept;
        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC EventEntryPayload final
    {
        std::vector<FuncArgInfo> parameters;

        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC NativeCallPayload final
    {
        std::shared_ptr<const NativeCallDefinition> definition;

        // Storage order remains execution, parameters, optional Self. Invocation puts Self first.
        [[nodiscard]] FlowNodeRegistration::PinResult describePins() const noexcept;
        [[nodiscard]] FlowForgeResult<std::vector<graph::PinSemanticId>> argumentSemantics() const noexcept;
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowNodeRegistration
    nativeCallRegistration(object::CodeLease code = object::CodeLease::builtin()) noexcept;

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC std::vector<FlowNodeRegistration> functionNodeRegistrations(
        object::CodeLease code = object::CodeLease::builtin()
    ) noexcept;
} // namespace lux::flowforge
