#pragma once

#include <lux/engine/material/MaterialCompileFailure.hpp>
#include <lux/engine/material/MaterialMath.hpp>
#include <lux/engine/material/ShaderIR.hpp>

#include <lux/cxx/compile_time/expected.hpp>
#include <span>

namespace lux::material::detail
{
    [[nodiscard]] std::size_t mathInputCount(EMathOp) noexcept;

    [[nodiscard]] EValueType mathOutputType(const MaterialMath&) noexcept;

    // Source/schema admission deliberately includes drafts not yet eligible for compilation.
    [[nodiscard]] cxx::expected<void, MaterialCompileFailure> validateMathPayload(const MaterialMath&) noexcept;

    [[nodiscard]] cxx::expected<void, MaterialCompileFailure> validateMath(const MaterialMath&) noexcept;

    [[nodiscard]] cxx::expected<std::uint32_t, MaterialCompileFailure>
    appendMath(const MaterialMath&, std::span<const std::uint32_t>, shadergen::ShaderIR&) noexcept;
} // namespace lux::material::detail
