#include <lux/engine/serialization/Serialization.hpp>

#include <cstdint>
#include <vector>

int
main()
{
    std::vector<std::byte> bytes;
    lux::serialization::BinaryWriter writer(bytes);
    const std::uint32_t value{};
    auto missing_budget_negative = lux::serialization::write(writer, value);
    return missing_budget_negative ? 0 : 1;
}
