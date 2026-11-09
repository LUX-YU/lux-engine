#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <cstdint>

namespace lux::simulation::script::detail
{
    struct LUX_COMPONENT(
        schema = "lux.simulation.script.ScriptAttachment", version = 1,
        snapshot = REBUILD, semantic = RUNTIME_DERIVED
    ) ScriptAttachment final
    {
        std::uint32_t mount_slot{};
    };
}

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/simulation/script/ScriptAttachment.type_static_info.hpp>
#endif
