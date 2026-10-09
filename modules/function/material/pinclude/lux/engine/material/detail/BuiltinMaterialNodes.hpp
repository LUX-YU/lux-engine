#pragma once

#include <lux/engine/material/BuiltinMaterialNodes.hpp>

namespace lux::material::detail
{
    [[nodiscard]] MaterialNodeResult<void> validateBuiltin(const MaterialConstant&) noexcept;
    [[nodiscard]] MaterialNodeResult<void> validateBuiltin(const MaterialInput&) noexcept;
    [[nodiscard]] MaterialNodeResult<void> validateBuiltin(const MaterialParameter&) noexcept;
    [[nodiscard]] MaterialNodeResult<void> validateBuiltin(const MaterialSwizzle&) noexcept;
    [[nodiscard]] MaterialNodeResult<void> validateBuiltin(const MaterialConstruct&) noexcept;

    // Inputs already have the declared scalar/vector type; coercion and dependency traversal belong
    // to graph lowering. These are the sole builtin emitters used by both graph and catalog paths.
    [[nodiscard]] MaterialNodeResult<std::uint32_t>
    appendBuiltin(const MaterialConstant&, std::span<const std::uint32_t>, shadergen::ShaderIR&) noexcept;

    [[nodiscard]] MaterialNodeResult<std::uint32_t>
    appendBuiltin(const MaterialInput&, std::span<const std::uint32_t>, shadergen::ShaderIR&) noexcept;

    [[nodiscard]] MaterialNodeResult<std::uint32_t>
    appendBuiltin(const MaterialSampleTexture&, std::span<const std::uint32_t>, shadergen::ShaderIR&) noexcept;

    [[nodiscard]] MaterialNodeResult<std::uint32_t>
    appendBuiltin(const MaterialParameter&, std::span<const std::uint32_t>, shadergen::ShaderIR&) noexcept;

    [[nodiscard]] MaterialNodeResult<std::uint32_t>
    appendBuiltin(const MaterialSwizzle&, std::span<const std::uint32_t>, shadergen::ShaderIR&) noexcept;

    [[nodiscard]] MaterialNodeResult<std::uint32_t>
    appendBuiltin(const MaterialConstruct&, std::span<const std::uint32_t>, shadergen::ShaderIR&) noexcept;

    [[nodiscard]] MaterialNodeResult<std::uint32_t>
    appendBuiltin(const MaterialDecodeNormal&, std::span<const std::uint32_t>, shadergen::ShaderIR&) noexcept;

    [[nodiscard]] MaterialNodeResult<std::uint32_t>
    appendBuiltin(const MaterialTbnTransform&, std::span<const std::uint32_t>, shadergen::ShaderIR&) noexcept;

    [[nodiscard]] MaterialNodeResult<void> appendSurface(std::span<const std::uint32_t>, shadergen::ShaderIR&) noexcept;
} // namespace lux::material::detail
