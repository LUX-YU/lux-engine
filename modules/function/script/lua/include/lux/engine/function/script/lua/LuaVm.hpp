#pragma once

#include <cstdint>
#include <string_view>

namespace lux::script::lua
{
    enum class ELuaExecutionPolicy : std::uint8_t
    {
        DEFAULT,
        INTERPRETER_ONLY,
    };

    struct LuaRuntimeInfo final
    {
        std::string_view vm;
        std::string_view version;
        bool jit_available{};
        bool jit_enabled{};
    };
} // namespace lux::script::lua
