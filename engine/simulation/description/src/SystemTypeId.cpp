#include <lux/engine/simulation/SystemTypeId.hpp>

namespace lux::simulation
{
    SystemTypeId systemTypeId(std::string_view canonical_name)
    {
        return SystemTypeId{
            lux::cxx::Fnv1a64::hash(canonical_name),
            std::string(canonical_name)};
    }
}
