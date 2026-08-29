#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/simulation/ecs/SystemTaskResources.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <array>
#include <cassert>

namespace
{
    lux::cxx::expected<void, lux::simulation::SystemBuildFailure> installProbe(
        lux::simulation::SimulationBuilder&,
        lux::simulation::SimulationSystemView
    ) noexcept
    {
        return {};
    }

    struct Position final
    {
        int value{};
    };

    struct ComponentWriterSystem final
    {
        inline static constexpr auto Access =
            lux::simulation::makeSystemAccessSpec<lux::simulation::ComponentWrite<Position>>();
        inline static constexpr lux::simulation::SystemDescription Description{
            .canonical_name = "lux.test.component-writer",
            .version = 1};
    };

    struct ComponentReaderSystem final
    {
        inline static constexpr auto Access =
            lux::simulation::makeSystemAccessSpec<lux::simulation::ComponentRead<Position>>();
        inline static constexpr lux::simulation::SystemDescription Description{
            .canonical_name = "lux.test.component-reader",
            .version = 1};
    };
}

int main()
{
    using namespace lux;

    simulation::SystemRegistry registry;
    const simulation::SystemRegistration first{
        simulation::systemTypeId("lux.test.first"),
        1U,
        &installProbe
    };
    const simulation::SystemRegistration second{
        simulation::systemTypeId("lux.test.second"),
        2U,
        &installProbe
    };

    assert(registry.add(first));
    assert(registry.size() == 1U);
    assert(registry.find(first.type) != nullptr);
    assert(registry.find(first.type)->version == 1U);
    assert(registry.find(second.type) == nullptr);

    auto duplicate = registry.add(first);
    assert(!duplicate);
    assert(duplicate.error().code == simulation::ESystemRegistrationError::DUPLICATE_TYPE);
    assert(registry.size() == 1U);

    const std::array atomic_batch{second, first};
    auto atomic_failure = registry.add(atomic_batch);
    assert(!atomic_failure);
    assert(registry.size() == 1U);
    assert(registry.find(second.type) == nullptr);

    assert(registry.add(second));
    assert(registry.size() == 2U);
    assert(registry.find(second.type)->version == 2U);

    assert(!registry.add(simulation::SystemRegistration{}));
    assert(registry.size() == 2U);

    task::TaskGraphBuilder builder;
    auto writer = builder.add(
        simulation::ecs::systemTaskResources<ComponentWriterSystem>(),
        []() noexcept {}
    );
    auto reader = builder.add(
        simulation::ecs::systemTaskResources<ComponentReaderSystem>(),
        []() noexcept {}
    );
    assert(writer && reader);
    auto graph = std::move(builder).build();
    assert(graph);
    assert(graph->dependencyCount() == 1U);

    return 0;
}
