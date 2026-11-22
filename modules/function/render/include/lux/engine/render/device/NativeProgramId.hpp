#pragma once

#include <cstddef>
#include <cstdint>

namespace lux::render::detail
{
    struct NativeProgramId final
    {
        std::size_t digest = 0;
        std::uint64_t publication = 0;

        [[nodiscard]] explicit operator bool() const noexcept { return publication != 0; }
        bool operator==(const NativeProgramId&) const noexcept = default;
    };
} // namespace lux::render::detail
