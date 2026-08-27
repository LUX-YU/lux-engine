#pragma once

#include <cstddef>
#include <span>

#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/UiIds.hpp>

namespace lux::ui
{
    /** Frame-borrowed view; invalid after the next UI frame or payload mutation. */
    struct DragDropPayloadView final
    {
        PayloadTypeIdView type;
        std::span<const std::byte> bytes;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return type.isValid();
        }
    };

    LUX_FUNCTION_PUBLIC void setDragDropPayload(PayloadTypeIdView type, std::span<const std::byte> bytes);
} // namespace lux::ui
