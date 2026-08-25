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

    const lux::ecs::WorldSectionLimits limits;
    assert(lux::ecs::worldSectionFormatVersion() == 2U);
    assert(lux::ecs::worldSectionLoaderContractVersion() == 1U);
    assert(limits.max_entities != 0U);
    assert(limits.max_columns != 0U);
    assert(limits.max_payload_bytes < (1ULL << 32U));
    return 0;
}
