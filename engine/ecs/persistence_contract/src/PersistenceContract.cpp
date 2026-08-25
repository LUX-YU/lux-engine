#include <lux/engine/ecs/persistence/ComponentPersistenceBinding.hpp>

namespace lux::ecs
{
    static_assert(sizeof(ComponentPersistenceBinding) <= 4U * sizeof(void*));

    std::uint32_t persistenceContractVersion() noexcept
    {
        return 1U;
    }
}
