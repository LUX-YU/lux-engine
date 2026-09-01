#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/SystemTaskResources.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <array>
#include <cassert>

namespace
{
    lux::cxx::expected<void, lux::simulation::SimulationSystemBuildFailure> installProbe(
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
        inline static constexpr lux::simulation::SimulationSystemDescription Description{
            .type = {
                .canonical_name = "lux.test.first",
                .version = 1
            }
        };
    };

    struct ComponentReaderSystem final
    {
        inline static constexpr auto Access =
            lux::simulation::makeSystemAccessSpec<lux::simulation::ComponentRead<Position>>();
        inline static constexpr lux::simulation::SimulationSystemDescription Description{
            .type = {
                .canonical_name = "lux.test.second",
                .version = 2
            }
        };
    };
}

int main()
{
    using namespace lux;

    simulation::SimulationSystemRegistry registry;
    const simulation::SimulationSystemRegistration first{
        .type = lux::system::systemTypeId("lux.test.first"),
        .cpp_type = cxx::typeToken<ComponentWriterSystem>(),
        .description = &ComponentWriterSystem::Description,
        .access = ComponentWriterSystem::Access.spec(),
        .configuration = serialization::noPortableValueCodec(),
        .install = &installProbe
    };
    const simulation::SimulationSystemRegistration second{
        .type = lux::system::systemTypeId("lux.test.second"),
        .cpp_type = cxx::typeToken<ComponentReaderSystem>(),
        .description = &ComponentReaderSystem::Description,
        .access = ComponentReaderSystem::Access.spec(),
        .configuration = serialization::noPortableValueCodec(),
        .install = &installProbe
    };

    assert(registry.add(first));
    assert(registry.size() == 1U);
    assert(registry.find(first.type) != nullptr);
    assert(registry.find(first.type)->description->type.version == 1U);
    assert(registry.find(second.type) == nullptr);

    auto duplicate = registry.add(first);
    assert(!duplicate);
    assert(duplicate.error().code == simulation::ESimulationSystemRegistrationError::DUPLICATE_TYPE);
    assert(registry.size() == 1U);

    const std::array atomic_batch{second, first};
    auto atomic_failure = registry.add(atomic_batch);
    assert(!atomic_failure);
    assert(registry.size() == 1U);
    assert(registry.find(second.type) == nullptr);

    assert(registry.add(second));
    assert(registry.size() == 2U);
    assert(registry.find(second.type)->description->type.version == 2U);

    assert(!registry.add(simulation::SimulationSystemRegistration{}));
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
