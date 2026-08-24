#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>

#include <array>
#include <cassert>
#include <memory>
#include <span>

namespace
{
    struct CloseProbe final
    {
        bool consumer_requested{false};
        bool consumer_complete{false};
        bool provider_requested{false};
    };

    class ProviderSystem final : public lux::ecs::ISystem
    {
    public:
        explicit ProviderSystem(CloseProbe& probe) noexcept : probe_(probe) {}

        void update(const lux::ecs::SystemUpdateContext&) override {}

        void requestClose() noexcept override
        {
            assert(probe_.consumer_complete);
            probe_.provider_requested = true;
        }

    private:
        CloseProbe& probe_;
    };

    class ConsumerSystem final : public lux::ecs::ISystem
    {
    public:
        explicit ConsumerSystem(CloseProbe& probe) noexcept : probe_(probe) {}

        [[nodiscard]] std::span<const Type> runsAfter() const noexcept override
        {
            static constexpr std::array<Type, 1u> kAfter{
                lux::ecs::systemType<ProviderSystem>()};
            return kAfter;
        }

        void update(const lux::ecs::SystemUpdateContext&) override
        {
            assert(probe_.consumer_requested);
            if (++owner_ticks_ == 2u)
                probe_.consumer_complete = true;
        }

        void requestClose() noexcept override
        {
            probe_.consumer_requested = true;
        }

        [[nodiscard]] bool closeComplete() const noexcept override
        {
            return probe_.consumer_complete;
        }

        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override
        {
            return probe_.consumer_requested && !probe_.consumer_complete;
        }

    private:
        CloseProbe& probe_;
        unsigned owner_ticks_{0u};
    };
}

int main()
{
    lux::ecs::World world;
    lux::ecs::Schedule schedule{world};
    CloseProbe probe;
    assert(schedule.addSystem(std::make_unique<ProviderSystem>(probe)));
    assert(schedule.addSystem(std::make_unique<ConsumerSystem>(probe)));
    assert(schedule.compile().valid());

    schedule.requestClose();
    assert(probe.consumer_requested);
    assert(!probe.provider_requested);
    auto state = schedule.closeState();
    assert(state.valid && !state.complete && state.pending_systems == 2u);

    schedule.tick(0.0f);
    assert(!probe.consumer_complete);
    assert(!probe.provider_requested);

    schedule.tick(0.0f);
    assert(probe.consumer_complete);
    assert(probe.provider_requested);
    state = schedule.closeState();
    assert(state.valid && state.complete && state.pending_systems == 0u);
}
