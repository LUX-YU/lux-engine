#include <lux/engine/serialization/Serialization.hpp>

namespace lux::serialization
{
    std::uint32_t binarySerializationContractVersion() noexcept
    {
        return 1U;
    }

    static_assert(Serializable<std::uint32_t>);
}
