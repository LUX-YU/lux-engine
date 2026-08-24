#include <lux/engine/ecs/World.hpp>

#include <cassert>
#include <cstdint>
#include <utility>

namespace
{
    struct Position final
    {
        int value{};
    };
}

int main()
{
    lux::ecs::World world;

    auto edit_result = world.edit();
    assert(edit_result);
    auto edit = std::move(*edit_result);

    const lux::ecs::Entity first = edit.create();
    edit.emplace<Position>(first, 7);
    assert(world.valid(first));
    assert(world.get<Position>(first).value == 7);

    auto rejected_edit = world.edit();
    assert(!rejected_edit);

    edit.destroy(first);
    assert(!world.valid(first));
    edit = {};

    auto next_edit_result = world.edit();
    assert(next_edit_result);
    auto next_edit = std::move(*next_edit_result);
    const lux::ecs::Entity second = next_edit.create();
    assert(world.valid(second));
    assert(second != first);

    next_edit.emplace<Position>(second, 3);
    world.patch<Position>(
        second,
        [](Position& position) noexcept
        {
            position.value = 11;
        }
    );
    assert(world.get<Position>(second).value == 11);

    std::size_t count{};
    for (auto [entity, position] : world.view<Position>().each())
    {
        assert(entity == second);
        assert(position.value == 11);
        ++count;
    }
    assert(count == 1);
}
