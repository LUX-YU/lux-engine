#pragma once

#include <lux/engine/function/script/ScriptApi.hpp>

#include <cstdint>

namespace lux::simulation::script
{
    struct ScriptApiCapabilityPublication final
    {
        lux::script::ScriptApiContractIdView contract;
        std::uint64_t schema_hash{};
        void* context{};
        const void* dispatch{};
    };

    struct PreparedScriptApiCapability final
    {
        lux::script::ScriptApiContractId contract;
        std::uint64_t schema_hash{};
        void* context{};
        const void* dispatch{};
    };
} // namespace lux::simulation::script
