#pragma once

#include <lux/cxx/core/StableNameId.hpp>

#include <cstdint>

namespace lux::script
{
    struct ScriptApiContractIdTag final
    {};

    struct ScriptApiMethodIdTag final
    {};

    using ScriptApiContractId = lux::cxx::StableNameId<ScriptApiContractIdTag>;
    using ScriptApiContractIdView = lux::cxx::StableNameIdView<ScriptApiContractIdTag>;
    using ScriptApiMethodId = lux::cxx::StableNameId<ScriptApiMethodIdTag>;
    using ScriptApiMethodIdView = lux::cxx::StableNameIdView<ScriptApiMethodIdTag>;

    enum class EScriptApiMethodKind : std::uint8_t
    {
        QUERY,
        COMMAND,
        ASYNC_OPERATION,
    };
} // namespace lux::script
