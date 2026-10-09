#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/material/MaterialCompileFailure.hpp>

namespace lux::material
{
    class MaterialGraph;

    namespace detail
    {
        [[nodiscard]] cxx::expected<void, MaterialCompileFailure> validateMaterialGraph(const MaterialGraph&) noexcept;
    }
} // namespace lux::material
