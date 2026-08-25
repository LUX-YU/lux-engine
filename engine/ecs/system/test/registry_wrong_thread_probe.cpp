#include <lux/engine/ecs/SystemContext.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>

#include <thread>

namespace
{
    class NoopSystem final : public lux::ecs::StaticSystemAccess<>
    {
    public:
        void update(lux::ecs::SystemContext&) noexcept {}
    };
}

int main()
{
    lux::ecs::SystemRegistry registry;
    const auto id = registry.emplace<NoopSystem>();
    if (!id)
        return 1;
    std::thread worker([&registry, id = *id]
    {
        (void)registry.erase(id);
    });
    worker.join();
    return 0;
}
