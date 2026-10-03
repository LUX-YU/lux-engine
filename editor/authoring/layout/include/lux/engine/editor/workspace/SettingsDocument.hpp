#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lux::editor::settings
{
    enum class ESettingsScope : std::uint8_t { INSTALLATION, PROJECT, USER, USER_PROJECT, LAUNCH };
    enum class ESettingsError : std::uint8_t
    {
        INVALID_DESCRIPTOR, DUPLICATE, COLLISION, INVALID_VALUE, INVALID_SCOPE, UNSUPPORTED_VERSION,
        CAPACITY, CONFLICT, UNAVAILABLE, CALLBACK, BUSY
    };
    struct SettingsFailure final
    {
        ESettingsError code;
        std::string detail;
    };
    template <class T> using SettingsResult = cxx::expected<T, SettingsFailure>;
    [[nodiscard]] constexpr std::uint32_t scopeBit(ESettingsScope scope) noexcept
    {
        return scope <= ESettingsScope::LAUNCH ? 1u << static_cast<unsigned>(scope) : 0u;
    }
    inline constexpr auto kPersonalScopes = scopeBit(ESettingsScope::INSTALLATION) |
        scopeBit(ESettingsScope::USER) | scopeBit(ESettingsScope::USER_PROJECT) | scopeBit(ESettingsScope::LAUNCH);

    struct SettingsValue final
    {
        std::string id;
        std::uint32_t schema{};
        std::vector<std::byte> bytes;
        friend bool operator==(const SettingsValue&, const SettingsValue&) = default;
    };
    struct SettingsDocument final
    {
        std::uint32_t schema{1};
        ESettingsScope scope{ESettingsScope::USER};
        std::vector<SettingsValue> values;
        // Read provenance, never encoded. The original file owner supplies this physical version.
        std::string file_version{"missing"};
        // Original TOML for value-preserving unknown fields; encoding overlays only known fields.
        // Comments/formatting are not a round-trip contract.
        std::string preserved;
    };
    struct SettingsLimits final
    {
        std::size_t file_bytes{1u << 20};
        std::size_t values{256};
        std::size_t depth{32};
    };
    [[nodiscard]] SettingsResult<std::vector<std::byte>>
    encodeSettings(const SettingsDocument&, SettingsLimits = {});
    [[nodiscard]] SettingsResult<SettingsDocument>
    decodeSettings(std::span<const std::byte>, SettingsLimits = {});

}
