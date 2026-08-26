#include "WorldSectionFixtureBuilder.hpp"

#include <lux/engine/ecs/WorldSectionTransaction.hpp>

#include <uuid.h>

#include <array>

int main()
{
    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 1U;
    auto image = lux::ecs::WorldSectionImage::open(
        lux::ecs::world_section::test::buildFixture(
            lux::ecs::WorldSectionId{uuids::uuid(id_bytes)},
            0U,
            {}
        ),
        lux::ecs::WorldSectionValidationBudget{0U, 0U, 0U, 1024U}
    );
    if (!image)
        return 1;
    lux::ecs::EcsState world{
        lux::ecs::EcsStateConfig{{4096U, 16U * 4096U}}
    };
    auto batch = lux::ecs::beginWorldSectionTransaction(
        world,
        lux::ecs::WorldSectionLoadScratchBudget{1024U},
        lux::serialization::SerializationLimits{}
    );
    if (!batch)
        return 2;
    lux::ecs::WorldSectionInstance output;
    if (!batch->load(lux::ecs::ComponentLoadSet{}, *image, output))
        return 3;
    lux::ecs::WorldSectionInstance invalid(std::move(output));
    (void)invalid;
    return 4;
}
