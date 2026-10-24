#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/InputEvent.hpp>

namespace lux::input
{
    struct InputSnapshot;
}

namespace lux::ui
{
    class Root;

    /// Translate one ordered input snapshot into Root's existing input path; no Editor shortcut policy.
    [[nodiscard]] LUX_FUNCTION_PUBLIC cxx::expected<void, EInputError> feedWindowInput(
        Root& root,
        const input::InputSnapshot& snapshot
    ) noexcept;
} // namespace lux::ui
