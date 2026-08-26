#include <lux/engine/ecs/WorldSectionContract.hpp>
#include <lux/engine/ecs/WorldSectionTransaction.hpp>
#include <lux/engine/ecs/WorldSectionTypes.hpp>

int main()
{
    const lux::ecs::WorldSectionValidationBudget validation{
        1024U,
        4096U,
        64U,
        1024U * 1024U,
    };
    lux::ecs::EcsState world({{4096U, 64U * 1024U}});
    auto transaction = lux::ecs::beginWorldSectionTransaction(
        world,
        lux::ecs::WorldSectionLoadScratchBudget{64U * 1024U},
        lux::serialization::SerializationLimits{}
    );
    return validation.max_columns == 64U && transaction &&
            lux::ecs::worldSectionFormatVersion() == 2U &&
            lux::ecs::worldSectionLoaderContractVersion() == 1U
        ? 0
        : 1;
}
