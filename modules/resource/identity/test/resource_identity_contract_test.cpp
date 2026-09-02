#include <lux/engine/resource/identity/AssetId.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <functional>

int main()
{
    using lux::asset::AssetId;

    assert(lux::asset::NullAssetId.isNull());
    std::array<std::uint8_t, 16U> bytes{};
    bytes.back() = 7U;
    const AssetId value{bytes};
    assert(!value.isNull());
    assert(std::to_integer<std::uint8_t>(value.bytes().back()) == 7U);
    assert(value == AssetId{bytes});
    assert(std::hash<AssetId>{}(value) == std::hash<AssetId>{}(AssetId{bytes}));
    return 0;
}
