#include <lux/engine/ecs/WorldSectionContract.hpp>
#include <lux/engine/ecs/WorldSectionTypes.hpp>

int main()
{
    const lux::ecs::WorldSectionLimits limits;
    return limits.max_columns != 0U &&
            lux::ecs::worldSectionFormatVersion() == 2U &&
            lux::ecs::worldSectionLoaderContractVersion() == 1U
        ? 0
        : 1;
}
