#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <cstdint>
#include <utility>

namespace
{
    class ObjectSystem final : public lux::object::Object<ObjectSystem>
    {
      public:
        inline static constexpr auto Access =
            lux::ecs::makeSystemAccessSpec<>();

        explicit ObjectSystem(std::uint32_t& count) noexcept : count_(&count) {}
        void update() noexcept { ++*count_; }

      private:
        std::uint32_t* count_{};
    };
}

int main()
{
    lux::ecs::SystemRegistry systems;
    std::uint32_t count{};
    const auto id = systems.emplace<ObjectSystem>(count);
    if (!id)
        return 1;
    auto retained = systems.retain<ObjectSystem>(*id);
    if (!retained)
        return 2;

    lux::task::TaskGraphBuilder builder;
    if (!builder.add(
            lux::task::on(lux::task::ETaskAffinity::CALLER_THREAD),
            [system = std::move(*retained)]() noexcept
            {
                system->update();
            }
        ))
    {
        return 3;
    }
    auto graph = std::move(builder).build();
    if (!graph)
        return 4;
    lux::task::TaskExecutor executor({0U, graph->taskCount()});
    if (!executor.execute(*graph))
        return 5;
    return count == 1U ? 0 : 6;
}
