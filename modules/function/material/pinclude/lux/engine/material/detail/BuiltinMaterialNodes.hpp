#pragma once

#include <lux/engine/material/BuiltinMaterialNodes.hpp>

namespace lux::material::detail
{
    [[nodiscard]] MaterialNodeResult<void> validateSwizzlePayload(const MaterialSwizzle&) noexcept;

    [[nodiscard]] MaterialNodeResult<void> validateBuiltin(const MaterialConstant&) noexcept;
    [[nodiscard]] MaterialNodeResult<void> validateBuiltin(const MaterialInput&) noexcept;
    [[nodiscard]] MaterialNodeResult<void> validateBuiltin(const MaterialParameter&) noexcept;
    [[nodiscard]] MaterialNodeResult<void> validateBuiltin(const MaterialSwizzle&) noexcept;
    [[nodiscard]] MaterialNodeResult<void> validateBuiltin(const MaterialConstruct&) noexcept;

} // namespace lux::material::detail
