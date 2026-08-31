#include <lux/engine/description/Visual.hpp>

#include <array>
#include <cassert>
#include <cstdint>

int main()
{
    using namespace lux;
    const asset::AssetId mesh{std::array<std::uint8_t, 16>{1U}};
    const asset::AssetId material{std::array<std::uint8_t, 16>{2U}};
    const rdesc::MeshVisualDescription visual{mesh, material, false, true, false};
    assert(visual.mesh == mesh);
    assert(visual.material == material);
    assert(!visual.visible && visual.cast_shadow && !visual.receive_shadow);

    rdesc::LightDescription light{};
    assert(light.type == rdesc::ELightType::POINT);
    assert((light.color == std::array<float, 3>{1.0F, 1.0F, 1.0F}));
    assert(light.cascade_splits.size() == rdesc::kLightCascadeSlots);
    return 0;
}
