#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace lux::asset::detail
{
    inline constexpr std::uint32_t kSpirvMagic = 0x07230203U;
    inline constexpr std::size_t kSpirvHeaderWords = 5U;

    [[nodiscard]] inline bool validSpirvWords(std::span<const std::uint32_t> words) noexcept
    {
        return words.size() >= kSpirvHeaderWords && words[0] == kSpirvMagic && words[1] != 0U &&
            words[3] != 0U && words[4] == 0U;
    }

    [[nodiscard]] inline bool validSpirvBytes(std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() < kSpirvHeaderWords * sizeof(std::uint32_t) ||
            bytes.size() % sizeof(std::uint32_t) != 0U)
        {
            return false;
        }
        std::uint32_t header[kSpirvHeaderWords]{};
        std::memcpy(header, bytes.data(), sizeof(header));
        return validSpirvWords(header);
    }
} // namespace lux::asset::detail
