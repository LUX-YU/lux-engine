#pragma once

#include <lux/engine/ui/ValueEdit.hpp>

namespace lux::ui::detail
{
    [[nodiscard]] constexpr EditResult atomicEditResult(bool changed) noexcept
    {
        return EditResult{changed, changed, changed, false};
    }
} // namespace lux::ui::detail
