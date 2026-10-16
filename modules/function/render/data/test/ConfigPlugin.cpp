#include "ConfigFixture.hpp"

extern "C" bool makeForeignConfig(
    const std::shared_ptr<const void>* pin, bool* alive, unsigned* destroyed, lux::render::ConfigValue* output
) noexcept
{
    std::vector<std::byte> bytes(3, std::byte{0x43});
    bytes.reserve(8192);
    auto value = lux::render::ConfigValue::make(config_test::ForeignConfig{std::move(bytes), *alive, *destroyed}, *pin);
    if (!value) return false;
    *output = std::move(*value);
    return true;
}
