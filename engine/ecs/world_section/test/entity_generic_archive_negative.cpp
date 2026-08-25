#include <lux/engine/ecs/Entity.hpp>

#include <lux/engine/serialization/BinaryWriter.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <vector>

int main()
{
    std::vector<std::byte> bytes;
    lux::serialization::BinaryWriter writer(bytes);
    const auto result = lux::serialization::write(
        writer,
        lux::ecs::Entity{}
    );
    (void)result;
}
