#include <lux/engine/ecs/WorldSectionContract.hpp>
#include <lux/engine/ecs/WorldSectionTypes.hpp>

#include <cassert>
#include <type_traits>

int main()
{
    static_assert(
        std::is_same_v<
            std::underlying_type_t<lux::ecs::EWorldSectionValueEncoding>,
            std::uint8_t>
    );
    static_assert(
        std::is_same_v<
            std::underlying_type_t<lux::ecs::EWorldSectionOrdinalEncoding>,
            std::uint8_t>
    );

    const lux::ecs::WorldSectionValidationBudget limits{
        1U,
        1U,
        1U,
        1U,
    };
    assert(lux::ecs::worldSectionFormatVersion() == 2U);
    assert(lux::ecs::worldSectionLoaderContractVersion() == 1U);
    assert(limits.max_entities == 1U);
    assert(limits.max_component_rows == 1U);
    assert(limits.max_columns == 1U);
    assert(limits.max_image_bytes == 1U);
    return 0;
}
