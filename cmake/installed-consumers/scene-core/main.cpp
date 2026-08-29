#include <lux/engine/scene/SceneDescription.hpp>
#include <type_traits>
int main()
{
    static_assert(std::is_aggregate_v<lux::scene::SceneDescription>);
    return 0;
}
