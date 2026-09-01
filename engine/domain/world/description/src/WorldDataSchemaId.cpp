#include <lux/engine/world/WorldDataSchemaId.hpp>

namespace lux::world
{
    WorldDataSchemaId worldDataSchemaId(std::string_view name)
    {
        return {lux::cxx::Fnv1a64::hash(name), std::string(name)};
    }
} // namespace lux::world
