#include "WorldSectionFixtureBuilder.hpp"

#include <lux/engine/ecs/WorldSectionLoader.hpp>

#include <uuid.h>

#include <array>

int main()
{
    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 2U;
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
    lux::ecs::World world;
    lux::ecs::WorldSectionInstance output;
    auto batch = lux::ecs::WorldSectionLoader::begin(
        world,
        lux::ecs::WorldSectionLoadScratchBudget{1024U},
        lux::serialization::SerializationLimits{}
    );
    if (!batch ||
        !batch->load(lux::ecs::ComponentLoadSet{}, *image, output) ||
        !batch->commit())
    {
        return 2;
    }
    return 0;
}
