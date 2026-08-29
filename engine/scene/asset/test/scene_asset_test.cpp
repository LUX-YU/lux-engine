#include <lux/engine/scene/SceneAssetCodec.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>

namespace
{
    [[nodiscard]] lux::asset::AssetId id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[15] = tail;
        return lux::asset::AssetId(bytes);
    }
}

int main()
{
    using namespace lux;
    const scene::SceneDescription source{id(1U), id(2U)};
    auto descriptor = scene::sceneAssetCodecDescriptor({});
    assert(descriptor.canonical_name == scene::SceneAssetCanonicalName);
    auto encoded = descriptor.encode(
        &source,
        asset::AssetEncodeContext{asset::AssetCodecLimits{0U, 0U, 40U}}
    );
    assert(encoded && encoded->size() == 40U);
    auto decoded = descriptor.decode(
        *encoded,
        asset::AssetDecodeContext{
            asset::AssetCodecLimits{40U, std::numeric_limits<std::size_t>::max(), 0U}
        }
    );
    assert(decoded);
    const auto scene_value = std::static_pointer_cast<const scene::SceneDescription>(decoded->payload);
    assert(scene_value->world == source.world);
    assert(scene_value->simulation == source.simulation);
    auto trailing = *encoded;
    trailing.push_back(std::byte{});
    assert(!descriptor.decode(
        trailing,
        asset::AssetDecodeContext{
            asset::AssetCodecLimits{41U, std::numeric_limits<std::size_t>::max(), 0U}
        }
    ));
    return 0;
}
