#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <utility>

namespace
{
    struct System final
    {
        inline static constexpr auto Access = lux::simulation::makeSystemAccessSpec<>();
        inline static constexpr lux::simulation::SystemDescription Description{
            .canonical_name = "lux.consumer.core-system",
            .version = 1};
        void update() noexcept
        {
            ++updates;
        }
        int updates{};
    };
}

int
main()
{
    lux::simulation::SystemRegistry systems;
    const auto id = systems.emplace<System>();
    if (!id)
        return 1;
    auto retained = systems.retain<System>(*id);
    if (!retained)
        return 2;

    lux::task::TaskGraphBuilder builder;
    auto observation = *retained;
    if (!builder.add([system = std::move(*retained)]() noexcept { system->update(); }))
    {
        return 3;
    }
    auto graph = std::move(builder).build();
    if (!graph)
        return 4;
    auto executor = lux::task::TaskExecutor::create({0U, graph->taskCount()});
    return executor && executor->execute(*graph) && observation->updates == 1 ? 0 : 5;
}
