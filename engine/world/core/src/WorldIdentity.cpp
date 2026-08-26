#include <lux/engine/world/WorldDataSchemaId.hpp>
#include <lux/engine/world/WorldObjectId.hpp>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<lux::world::WorldObjectId>);

lux::world::WorldDataSchemaId lux::world::worldDataSchemaId(
    std::string_view name
)
{
    return {
        lux::cxx::Fnv1a64::hash(name),
        std::string(name)
    };
}
