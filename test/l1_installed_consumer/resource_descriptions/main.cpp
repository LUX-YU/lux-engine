#include <lux/engine/description/Material.hpp>
#include <lux/engine/description/Model.hpp>
#include <lux/engine/description/TextureAtlas.hpp>

#include <array>
#include <cstdint>

namespace
{
    lux::asset::AssetId id(std::uint8_t value)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = value;
        return lux::asset::AssetId{bytes};
    }
}

int main()
{
    lux::rdesc::MaterialDescription material;
    material.texture_slot_ids[0] = id(1U);
    lux::rdesc::ModelDescription model;
    model.primitives.push_back({id(2U), id(3U)});
    model.nodes.push_back({});
    model.nodes.front().primitives.push_back(0U);
    lux::rdesc::TextureAtlas atlas;
    atlas.texture = id(1U);
    return material.texture_slot_ids[0].isNull() || model.primitives[0].mesh.isNull() || atlas.texture.isNull();
}
