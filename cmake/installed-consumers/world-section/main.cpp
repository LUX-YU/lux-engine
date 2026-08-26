#include <lux/engine/ecs/ComponentLoadSet.hpp>
#include <lux/engine/ecs/WorldSectionImage.hpp>
#include <lux/engine/ecs/WorldSectionTransaction.hpp>

#include <vector>

int main()
{
    lux::ecs::ComponentLoadSet loads;
    auto image = lux::ecs::WorldSectionImage::open(
        std::vector<std::byte>{}
    );
    return !loads.empty() || image ? 1 : 0;
}
