#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <cstdint>
#include <utility>

namespace
{
    class CountSystem final
    {
    public:
        inline static constexpr auto Access = lux::simulation::makeSystemAccessSpec<>();
        inline static constexpr lux::simulation::SystemDescription Description{
            .canonical_name = "lux.consumer.l1-core-system",
            .version = 1};

        explicit CountSystem(std::uint32_t& count) noexcept : count_(&count)
        {
        }
        void update() noexcept
        {
            ++*count_;
        }

    private:
        std::uint32_t* count_{};
    };
}

int
main()
{
    lux::simulation::SystemRegistry systems;
    std::uint32_t count{};
    const auto first = systems.emplace<CountSystem>(count);
    const auto second = systems.emplace<CountSystem>(count);
    if (!first || !second)
        return 1;

    auto first_system = systems.retain<CountSystem>(*first);
    auto second_system = systems.retain<CountSystem>(*second);
    if (!first_system || !second_system)
        return 2;

    lux::task::TaskGraphBuilder builder;
    if (!builder.add([system = std::move(*first_system)]() noexcept { system->update(); }) ||
        !builder.add([system = std::move(*second_system)]() noexcept { system->update(); }))
    {
        return 3;
    }
    auto graph = std::move(builder).build();
    if (!graph)
        return 4;

    auto executor = lux::task::TaskExecutor::create({0U, graph->taskCount()});
    if (!executor)
        return 5;
    if (!executor->execute(*graph))
        return 6;
    return count == 2U ? 0 : 7;
}
