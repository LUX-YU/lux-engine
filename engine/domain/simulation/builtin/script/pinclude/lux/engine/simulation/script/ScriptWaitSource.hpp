#pragma once

#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>

namespace lux::simulation::script::detail
{
    struct ScriptSourceId final
    {
        std::uint32_t slot{};
        std::uint32_t generation{};
        [[nodiscard]] constexpr bool valid() const noexcept { return slot != 0U && generation != 0U; }
        friend constexpr bool operator==(ScriptSourceId, ScriptSourceId) noexcept = default;
    };

    enum class EScriptWaitSource : std::uint8_t
    {
        NONE,
        EVENT,
        TIMER,
    };

    struct ScriptWaitSource final
    {
        ScriptSourceId id;
        EScriptWaitSource kind{EScriptWaitSource::NONE};
        friend constexpr bool operator==(ScriptWaitSource, ScriptWaitSource) noexcept = default;
    };

    struct ScriptSourceCancellation final
    {
        ScriptInstanceId instance;
        ScriptAwaitableId awaitable;
        ScriptWaitSource source;
    };

    struct ScriptTimerAssociation final
    {
        ScriptInstanceId instance;
        ScriptAwaitableId awaitable;
    };
}
