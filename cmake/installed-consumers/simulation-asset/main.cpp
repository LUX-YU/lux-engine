#include <lux/engine/simulation/SimulationAssetCodec.hpp>

#include <type_traits>

int
main()
{
    static_assert(std::is_same_v<
        lux::simulation::SimulationAsset::data_type,
        lux::simulation::SimulationDescription
    >);
    static_assert(lux::simulation::SimulationAsset::asset_type ==
        lux::asset::AssetTypeId::fromName("lux.simulation.description"));
    return lux::simulation::SimulationAsset::primary_magic ==
        lux::simulation::SimulationAssetPrimaryMagic ? 0 : 1;
}
