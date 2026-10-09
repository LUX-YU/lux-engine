#pragma once

#include <lux/engine/flowforge/FlowForgeFailure.hpp>
#include <lux/engine/flowforge/visibility.h>
#include <lux/engine/function/graph/GraphTypes.hpp>

#include <span>

namespace lux::meta
{
    struct RefField;
}

namespace lux::flowforge
{
    class NativeCallDefinition;
    class ScriptAbilityPayload;
    class ScriptEventPayload;

    // Borrowed during one registered execution-node compilation. Semantics identify pins of that
    // node's declared schema. The backend owns SSA, region scopes, token threading and traversal;
    // neither the callback nor this interface owns or mutates an authoring graph.
    class LUX_ENGINE_FLOWFORGE_PUBLIC FlowExecutionCompiler
    {
    public:
        virtual ~FlowExecutionCompiler() = default;

        [[nodiscard]] virtual FlowForgeResult<void> branch(
            graph::PinSemanticId condition,
            graph::PinSemanticId true_leg,
            graph::PinSemanticId false_leg
        ) noexcept = 0;

        // The stable callee identity is resolved against the current graph by the backend.
        // Instance state and the Script Ability runtime remain hidden call arguments.
        [[nodiscard]] virtual FlowForgeResult<void> functionCall(
            graph::NodeId callee,
            std::span<const graph::PinSemanticId> arguments,
            std::span<const graph::PinSemanticId> results,
            graph::PinSemanticId completed
        ) noexcept = 0;

        [[nodiscard]] virtual FlowForgeResult<void> abilityCall(
            const ScriptAbilityPayload& ability,
            std::span<const graph::PinSemanticId> arguments,
            std::span<const graph::PinSemanticId> results,
            graph::PinSemanticId completed
        ) noexcept = 0;

        [[nodiscard]] virtual FlowForgeResult<void> eventWait(
            const ScriptEventPayload& event,
            graph::PinSemanticId payload,
            graph::PinSemanticId completed
        ) noexcept = 0;

        [[nodiscard]] virtual FlowForgeResult<void> storeVariable(
            std::uint64_t variable,
            graph::PinSemanticId value,
            graph::PinSemanticId result,
            graph::PinSemanticId completed
        ) noexcept = 0;

        [[nodiscard]] virtual FlowForgeResult<void> storeField(
            const meta::RefField& field,
            graph::PinSemanticId object,
            graph::PinSemanticId value,
            graph::PinSemanticId result,
            graph::PinSemanticId completed
        ) noexcept = 0;

        // Bounds are [first,last). The index output is defined only in the loop body.
        [[nodiscard]] virtual FlowForgeResult<void> forLoop(
            graph::PinSemanticId first,
            graph::PinSemanticId last,
            graph::PinSemanticId index,
            graph::PinSemanticId body,
            graph::PinSemanticId completed
        ) noexcept = 0;

        // Re-evaluate the condition within its own loop region, including memory reads.
        [[nodiscard]] virtual FlowForgeResult<void> whileLoop(
            graph::PinSemanticId condition,
            graph::PinSemanticId body,
            graph::PinSemanticId completed
        ) noexcept = 0;

        // Thread each completed leg's token into the next. Return/Break stops remaining legs.
        [[nodiscard]] virtual FlowForgeResult<void> sequence(std::span<const graph::PinSemanticId>) noexcept = 0;

        [[nodiscard]] virtual FlowForgeResult<void> returnValues(std::span<const graph::PinSemanticId>) noexcept = 0;

        [[nodiscard]] virtual FlowForgeResult<void> breakLoop() noexcept = 0;

        // Arguments are in invocation order (Self first for methods), not schema storage order.
        // Definition and semantics are borrowed only for this synchronous compilation.
        [[nodiscard]] virtual FlowForgeResult<void> nativeCall(
            const NativeCallDefinition& definition,
            std::span<const graph::PinSemanticId> arguments,
            graph::PinSemanticId result,
            graph::PinSemanticId completed
        ) noexcept = 0;
    };
} // namespace lux::flowforge
