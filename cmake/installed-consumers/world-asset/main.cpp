#include <lux/engine/world/WorldAssetCodec.hpp>

#include <memory>

int
main()
{
    auto pin = std::make_shared<int>(1);
    const auto descriptor = lux::world::worldAssetCodecDescriptor(pin);
    return descriptor.primary_magic == lux::world::WorldAssetPrimaryMagic ? 0 : 1;
}
