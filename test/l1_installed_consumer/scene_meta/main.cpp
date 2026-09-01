#include <lux/engine/scene/SceneMetaManager.hpp>

#include <type_traits>
#include <span>
#include <utility>

int main()
{
    static_assert(!std::is_copy_constructible_v<lux::scene::SceneMetaManager>);
    static_assert(std::is_move_constructible_v<lux::scene::SceneMetaManager>);
    using Query = decltype(std::declval<const lux::scene::SceneMetaManager&>().allSystems());
    static_assert(std::is_same_v<Query, std::span<const lux::scene::SystemMetaView>>);
    return 0;
}
