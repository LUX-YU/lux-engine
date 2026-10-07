#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace lux::project
{
    // Plugin declarations and project selections share [A-Za-z_][A-Za-z0-9_.-]*.
    [[nodiscard]] constexpr bool isMetadataName(std::string_view name) noexcept
    {
        const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
        if (name.empty() || !letter(name.front()))
        {
            return false;
        }
        for (const char c : name)
        {
            const bool is_valid_character = letter(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
            if (!is_valid_character)
            {
                return false;
            }
        }
        return true;
    }
    struct MetadataIdentity final
    {
        std::string id;
        std::uint32_t version{};
        friend bool operator==(const MetadataIdentity&, const MetadataIdentity&) = default;
    };
} // namespace lux::project
