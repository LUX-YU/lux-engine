#pragma once

#include <cstdint>
#include <string_view>

namespace lux::script::lua
{
    struct LuaRuntimeInfo final
    {
        std::string_view vm;
        std::string_view version;
    };
} // namespace lux::script::lua
