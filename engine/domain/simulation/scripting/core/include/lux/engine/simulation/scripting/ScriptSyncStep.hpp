#pragma once

#include <lux/engine/function/script/BoundScriptCall.hpp>
#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>

namespace lux::simulation::script
{
    class ScriptBehavior;

    enum class EScriptSyncStepError : std::uint8_t
    {
        INVALID_CONTEXT,
        INVALID_STEP,
        SIGNATURE_MISMATCH,
        INVOCATION_REVOKED,
        BACKEND_FAILURE,
    };

    struct ScriptSyncStepError final
    {
        std::uint32_t ordinal{};
        EScriptSyncStepError category{EScriptSyncStepError::INVALID_CONTEXT};
        std::int32_t status{-1};
    };

    struct PreparedScriptSyncStep final
    {
        const lux::rdesc::ScriptFunction* signature{};
        lux::script::BoundScriptCall call;
    };

    // Immutable publication borrowed by a backend child. Its owner outlives every child task.
    // Neither this view nor a prepared entry conveys permission to invoke user code.
    struct ScriptSyncStepSetView final
    {
        ScriptInstanceId instance;
        std::uint64_t publication{};
        ScriptBehavior* behavior{};
        std::span<const PreparedScriptSyncStep> steps;
        const void* owner{};
        bool (*current)(const void*, ScriptInstanceId, std::uint64_t) noexcept{};
    };
}
