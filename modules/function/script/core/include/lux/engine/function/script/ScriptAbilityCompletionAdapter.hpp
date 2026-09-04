#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>
#include <lux/engine/function/script/ScriptAbilityAsync.hpp>

#include <cstdint>
#include <utility>

namespace lux::script
{
    template <class Result>
    [[nodiscard]] lux::cxx::expected<ScriptAbilityCompletion<Result>, ScriptAbilityOperationError>
    adaptScriptAbilityCompletion(ScriptAbilityErasedCompletion completion) noexcept
    {
        if (!completion)
        {
            return lux::cxx::unexpected<ScriptAbilityOperationError>(ScriptAbilityOperationError{
                static_cast<std::int32_t>(EScriptAbilityErasedCallStatus::INVALID_ARGUMENTS)
            });
        }
        return ScriptAbilityCompletion<Result>::fromErased(std::move(completion));
    }
} // namespace lux::script
