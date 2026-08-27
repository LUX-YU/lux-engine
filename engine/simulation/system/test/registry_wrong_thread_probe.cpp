#include <lux/engine/simulation/SystemRegistry.hpp>

#include <thread>

namespace
{
    class NoopSystem final
    {
    public:
        inline static constexpr auto Access = lux::simulation::ecs::makeSystemAccessSpec<>();
    };
}

int
main()
{
    lux::simulation::ecs::SystemRegistry registry;
    const auto id = registry.emplace<NoopSystem>();
    if (!id)
        return 1;
    std::thread worker([&registry, id = *id] { (void)registry.erase(id); });
    worker.join();
    return 0;
}
