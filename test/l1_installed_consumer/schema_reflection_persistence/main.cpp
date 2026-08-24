#include <lux/engine/ecs/WorldSection.hpp>
#include <lux/engine/ecs/reflection/ComponentReflectionAdapter.hpp>

#include <array>
#include <cstddef>

int main()
{
    const auto codec_factory = &lux::ecs::reflectedComponentCodec;
    const std::array<std::byte, 4> invalid_image{};
    const auto decoded = lux::ecs::decodeWorldSection(invalid_image);
    return codec_factory != nullptr && !decoded ? 0 : 1;
}
