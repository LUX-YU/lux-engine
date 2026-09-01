#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/material/compiler/MaterialIR.hpp>

namespace lux::material::compiler
{
    [[nodiscard]] lux::cxx::expected<MaterialIR, MaterialCompileFailure>
    lowerMaterial(const MaterialGraph& graph);
} // namespace lux::material::compiler
