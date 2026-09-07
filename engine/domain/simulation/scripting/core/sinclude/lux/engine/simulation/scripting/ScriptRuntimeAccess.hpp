#pragma once

#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <optional>

namespace lux::simulation::script::detail
{
    class ScriptRuntimeAccess final
    {
    public:
        static void attach(ScriptBehavior& behavior, ScriptInstanceScope scope, const ScriptHostApi& host) noexcept
        {
            behavior.attach(scope, host);
        }

        [[nodiscard]] static ScriptEventAdmissionHandle admission(
            ScriptEventAdmissionScope scope, ScriptInstanceId instance, std::uint64_t epoch, std::uint32_t local
        ) noexcept
        {
            ScriptEventAdmissionHandle result;
            result.scope_ = scope;
            result.instance_ = instance;
            result.layout_epoch_ = epoch;
            result.local_slot_ = local;
            return result;
        }

        [[nodiscard]] static std::optional<std::uint32_t> matchAdmission(
            ScriptEventAdmissionHandle handle,
            ScriptEventAdmissionScope scope,
            ScriptInstanceId instance,
            std::uint64_t epoch,
            std::size_t count
        ) noexcept
        {
            const bool matches = handle.scope_ == scope && handle.instance_ == instance &&
                handle.layout_epoch_ == epoch && handle.local_slot_ < count;
            return matches ? std::optional{handle.local_slot_} : std::nullopt;
        }
    };
}
