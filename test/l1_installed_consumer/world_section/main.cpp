#include <lux/engine/ecs/WorldSectionContract.hpp>
#include <lux/engine/ecs/WorldSectionLoader.hpp>
#include <lux/engine/ecs/WorldSectionTypes.hpp>

int main()
{
    const lux::ecs::WorldSectionValidationBudget validation{
        1024U,
        4096U,
        64U,
        1024U * 1024U,
    };
    lux::ecs::World world;
    auto batch = lux::ecs::WorldSectionLoader::begin(
        world,
        lux::ecs::WorldSectionLoadScratchBudget{64U * 1024U},
        lux::serialization::SerializationLimits{}
    );
    return validation.max_columns == 64U && batch &&
            lux::ecs::worldSectionFormatVersion() == 2U &&
            lux::ecs::worldSectionLoaderContractVersion() == 1U
        ? 0
        : 1;
}
