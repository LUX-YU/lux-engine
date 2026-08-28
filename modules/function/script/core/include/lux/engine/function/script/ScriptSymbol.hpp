#pragma once

#include <cstdint>

namespace lux::script
{
    using ScriptSymbolId = std::uint64_t;
    inline constexpr ScriptSymbolId InvalidScriptSymbolId{};
}
