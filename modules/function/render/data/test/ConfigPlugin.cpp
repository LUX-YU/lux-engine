#include "ConfigFixture.hpp"

extern "C" bool makeForeignConfig(
    bool* alive, unsigned* destroyed, lux::render::ConfigValue* output
) noexcept
{
    std::vector<std::byte> bytes(3, std::byte{0x43});
    bytes.reserve(8192);
    auto value = lux::render::ConfigValue::make(config_test::ForeignConfig{std::move(bytes), *alive, *destroyed});
    if (!value) return false;
    *output = std::move(*value);
    return true;
}
