#pragma once

#include <cstddef>
#include <span>

namespace lux::engine::platform
{
    // OS entropy for cold process/session identities. No deterministic fallback on failure.
    [[nodiscard]] bool fillRandomBytes(std::span<std::byte> destination) noexcept;
} // namespace lux::engine::platform
