#include <lux/engine/resource/identity/AssetId.hpp>

#include <array>
#include <cstdint>
#include <functional>

int main()
{
    std::array<std::uint8_t, 16U> bytes{};
    bytes.back() = 3U;
    const lux::asset::AssetId id{bytes};
    const auto hash = std::hash<lux::asset::AssetId>{}(id);
    return id.isNull() || hash != std::hash<lux::asset::AssetId>{}(lux::asset::AssetId{bytes}) ? 1 : 0;
}
