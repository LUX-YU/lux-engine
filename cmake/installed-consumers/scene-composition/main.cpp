#include <lux/engine/scene/Scene.hpp>
#include <type_traits>
int main()
{
    static_assert(!std::is_copy_constructible_v<lux::scene::SceneDescription>);
    static_assert(!std::is_move_constructible_v<lux::scene::Scene>);
    return 0;
}
