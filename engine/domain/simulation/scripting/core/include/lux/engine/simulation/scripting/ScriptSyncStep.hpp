#pragma once

#include <lux/engine/function/script/BoundScriptCall.hpp>
#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>

namespace lux::simulation::script
{
    class ScriptBehavior;
    class ScriptInvocationValidity;

    // Static native call-site shapes, owned by the same executable as its task
    // contract. A shape is matched to the actual Lua export during preparation, not
    // on each call.
    struct ScriptSyncStepType final
    {
        lux::semantic::Layout layout;
        lux::semantic::EValuePass pass{};
    };

    struct ScriptSyncStepShape final
    {
        std::span<const ScriptSyncStepType> arguments;
        std::span<const ScriptSyncStepType> results;
    };

    namespace detail
    {
        [[nodiscard]] inline bool syncStepShapeMatches(const ScriptSyncStepShape& shape,
                                                       const lux::rdesc::ScriptFunction& function) noexcept
        {
            const bool invalid_count =
                shape.arguments.size() != function.args.size() || shape.results.size() != function.returns.size();
            if (invalid_count)
                return false;
            const auto matches = [](const ScriptSyncStepType& type, const lux::rdesc::ScriptValueType& actual)
            {
                return type.layout.type_id == actual.type_id && type.layout.canonical_name == actual.canonical_name &&
                       type.layout.abi_kind == actual.abi_kind && type.layout.size == actual.size &&
                       type.layout.alignment == actual.alignment && type.pass == actual.pass;
            };
            for (std::size_t i{}; i < shape.arguments.size(); ++i)
                if (!matches(shape.arguments[i], function.args[i]))
                    return false;
            for (std::size_t i{}; i < shape.results.size(); ++i)
                if (!matches(shape.results[i], function.returns[i]))
                    return false;
            return true;
        }
    } // namespace detail

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
        const ScriptSyncStepShape* shape{};
        void* context{};
        // Only the typed bridge supplies these native addresses after checking the
        // cold shape identity. The original qualification must be checked after any
        // conversion that can reenter.
        std::int32_t (*invoke)(void*, const void* const*, void*, const ScriptInvocationValidity&) noexcept {};
    };

    // Immutable publication borrowed by a backend child. Its owner outlives every
    // child task. Neither this view nor a prepared entry conveys permission to
    // invoke user code.
    struct ScriptSyncStepSetView final
    {
        ScriptInstanceId instance;
        std::uint64_t publication{};
        ScriptBehavior* behavior{};
        std::span<const PreparedScriptSyncStep> steps;
        const void* owner{};
        bool (*current)(const void*, ScriptInstanceId, std::uint64_t) noexcept{};
    };
} // namespace lux::simulation::script
