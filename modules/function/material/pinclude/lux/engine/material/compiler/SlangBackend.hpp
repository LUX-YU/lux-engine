#pragma once

#include <lux/engine/material/compiler/MaterialCompiler.hpp>

namespace lux::material::detail
{
    [[nodiscard]] cxx::expected<std::string, MaterialDeriveFailure>
    emitSlang(const MaterialIR& ir, rdesc::EMaterialPurpose purpose);
}
