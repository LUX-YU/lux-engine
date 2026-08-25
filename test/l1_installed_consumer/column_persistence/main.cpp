#include <lux/engine/ecs/WorldSection.hpp>

#include <array>
#include <cstddef>

int main()
{
    const std::array<std::byte, 4> old_wire{
        std::byte{'L'}, std::byte{'X'}, std::byte{'W'}, std::byte{'S'}};
    const auto decoded = lux::ecs::decodeWorldSection(old_wire);
    return !decoded && decoded.error().code ==
        lux::ecs::EPersistenceError::INVALID_MAGIC
        ? 0
        : 1;
}
