#include <lux/engine/ecs/WorldSectionContract.hpp>
#include <lux/engine/ecs/WorldSectionTypes.hpp>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<lux::ecs::WorldSectionId>);

std::uint32_t lux::ecs::worldSectionFormatVersion() noexcept
{
    return 2U;
}

std::uint32_t lux::ecs::worldSectionLoaderContractVersion() noexcept
{
    return 1U;
}
