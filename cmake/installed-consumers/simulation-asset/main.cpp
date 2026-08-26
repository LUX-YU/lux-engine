#include <lux/engine/simulation/SimulationAssetCodec.hpp>

#include <memory>

int main()
{
    auto pin = std::make_shared<int>(1);
    const auto descriptor = lux::simulation::simulationAssetCodecDescriptor(pin);
    return descriptor.primary_magic ==
        lux::simulation::SimulationAssetPrimaryMagic ? 0 : 1;
}
