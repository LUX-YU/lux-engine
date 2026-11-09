#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lux::rdesc
{
    // Opaque application token. Zero selects the UI font atlas. Render resolves tokens against the compiled image selections;
    // this value owns no image and contains no Renderer/ImGui type or callback.
    struct UiImageToken final
    {
        std::uint64_t value = 0;
        auto operator<=>(const UiImageToken&) const noexcept = default;
    };

} // namespace lux::rdesc
