#include <lux/engine/simulation/systems/TransformSystem.hpp>

int main()
{
    const auto configuration = lux::simulation::makeTransformSystemConfiguration(
        1024U,
        lux::simulation::ecs::EcsCommandProducerCapacity{2048U, 256U * 1024U}
    );
    const auto registrations = lux::simulation::transformSystemRegistrations();
    return configuration && configuration->size() == 24U && registrations.size() == 1U &&
        registrations[0].version == lux::simulation::transformSystemDescription().version ? 0 : 1;
}
