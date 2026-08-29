#include <lux/engine/simulation/ecs/Transform.hpp>

#include <concepts>

int main()
{
    using namespace lux::simulation::ecs;
    static_assert(std::same_as<typename decltype(Transform2D{}.translation)::Scalar, double>);
    static_assert(std::same_as<typename decltype(Transform3D{}.translation)::Scalar, double>);
    static_assert(std::same_as<typename decltype(WorldTransform2D{}.value)::Scalar, double>);
    static_assert(std::same_as<typename decltype(WorldTransform3D{}.value)::Scalar, double>);
    return 0;
}
