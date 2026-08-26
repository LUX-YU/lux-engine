#include <lux/engine/ecs/ComponentLoadSet.hpp>
#include <lux/engine/ecs/WorldSectionImage.hpp>
#include <lux/engine/ecs/WorldSectionTransaction.hpp>

#include <type_traits>
#include <vector>

int main()
{
    lux::ecs::ComponentLoadSet loads;
    const lux::ecs::WorldSectionValidationBudget budget{
        1024U,
        4096U,
        64U,
        1024U * 1024U,
    };
    auto image = lux::ecs::WorldSectionImage::open(
        std::vector<std::byte>{},
        budget
    );
    static_assert(
        !std::is_move_assignable_v<lux::ecs::WorldSectionTransaction>
    );
    return !loads.empty() || image ? 1 : 0;
}
