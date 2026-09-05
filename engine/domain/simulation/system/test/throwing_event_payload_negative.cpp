#include <lux/engine/simulation/HookChannel.hpp>

struct ThrowingEventPayload final
{
    ThrowingEventPayload() noexcept = default;
    ThrowingEventPayload(const ThrowingEventPayload&)
    {
    }
    ThrowingEventPayload(ThrowingEventPayload&&) noexcept = default;
    ~ThrowingEventPayload() noexcept = default;
};

int main()
{
    lux::simulation::HookChannel<lux::simulation::SimulationBroadcastRoute, ThrowingEventPayload> endpoint;
    return endpoint.handlerCount() == 0U ? 0 : 1;
}
