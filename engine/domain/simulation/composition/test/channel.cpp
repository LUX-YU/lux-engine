#include <lux/engine/simulation/HookChannel.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemInstaller.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <limits>

using namespace lux;
using namespace lux::simulation;
using Channel = THookChannel<SimulationBroadcastRoute, std::int32_t>;

static_assert(!std::is_default_constructible_v<Channel>);
static_assert(!std::is_move_constructible_v<Channel>);
static_assert(!std::is_copy_constructible_v<Channel>);

namespace
{
    void standalone()
    {
        auto created = Channel::create({1, 2, 2});
        assert(created);
        auto channel = std::move(*created);
        assert(channel->mutationError() == EEndpointMutationError::NONE);
        assert(!channel->begin(1).record(99));
        {
            auto writer = channel->begin(0);
            assert(writer.record(11));
            assert(!channel->begin(0).record(99));
            assert(channel->mutationError() == EEndpointMutationError::WRITER_ACTIVE);
            assert(!channel->seal());
            auto moved = std::move(writer);
            assert(!writer.record(99));
            assert(moved.record(12));
        }
        assert(channel->seal());
        assert(channel->mutationError() == EEndpointMutationError::DISPATCH_ACTIVE);
        assert(!channel->begin(0).record(99));
        assert(channel->lane(0).size() == 2 && channel->lane(0)[0].payload == 11);
        assert(channel->lane(0)[1].payload == 12 && channel->lane(1).empty());
        {
            auto owner = channel->beginOwner();
            assert(owner.record(33));
        }
        channel->reset();
        assert(channel->pendingOccurrenceCount() == 1);
        assert(channel->seal());
        assert(channel->lane(0).empty());
        assert(channel->lane(1).size() == 1 && channel->lane(1)[0].payload == 33);
        channel->discard();
        assert(channel->pendingOccurrenceCount() == 0);
        {
            auto writer = channel->begin(0);
            assert(writer.record(1) && writer.record(2));
            assert(!writer.record(3));
        }
        assert(channel->failed() && !channel->seal());
        channel->discard();
        assert(!channel->failed() && channel->seal());
        channel->discard();

        assert(Channel::create({0, 1, 0}).error() == EEndpointMutationError::CAPACITY_EXCEEDED);
        assert(Channel::create({1, 0, 0}).error() == EEndpointMutationError::CAPACITY_EXCEEDED);
        assert(
            Channel::create({(std::numeric_limits<std::size_t>::max)(), 2, 0}).error() ==
            EEndpointMutationError::CAPACITY_EXCEEDED
        );

        struct Owned final
        {
            std::shared_ptr<int> value;
        };

        using OwnedChannel = THookChannel<SimulationBroadcastRoute, Owned>;
        assert(OwnedChannel::create({1, 1, 0}).error() == EEndpointMutationError::PAYLOAD_NOT_OWNED);
        auto owned_result = OwnedChannel::create({1, 1, 0}, [](const Owned& value) noexcept { return value; });
        assert(owned_result);
        auto owned = std::move(*owned_result);
        auto lifetime = std::make_shared<int>(7);
        std::weak_ptr<int> weak = lifetime;
        {
            auto writer = owned->begin(0);
            assert(writer.record(Owned{std::move(lifetime)}));
        }
        assert(!weak.expired());
        owned->discard();
        assert(weak.expired());
    }

    struct Probe final
    {
        inline static constexpr std::string_view Worlds[]{"*"};
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr std::array Hooks{makeHookPointSpec<void()>({1}, "deliver", true, false)};
        inline static constexpr std::array Events{makeEventPointSpec<
            std::int32_t>({1}, "records", {1}, EEventRoute::SIMULATION_BROADCAST, "lux.lr07.channel.record", 1, true)};
        inline static constexpr SimulationSystemDescription Description{
            {.canonical_name = "lux.lr07.ChannelProbe", .version = 1, .supported_world_types = Worlds},
            Hooks,
            Events
        };
        inline static std::size_t deliveries{};
        Channel* channel{};
        Channel::Producer producer;
    };

    void composed()
    {
        SimulationSystemRegistry types;
        assert(types.add(
            {.type = system::systemTypeId(Probe::Description.type.canonical_name),
             .cpp_type = cxx::typeToken<Probe>(),
             .description = &Probe::Description,
             .access = Probe::Access.spec(),
             .install = [](SimulationSystemInstaller& installer, SimulationSystemView description
                        ) noexcept -> cxx::expected<void, SimulationSystemBuildFailure>
             {
                 const auto id = description.instanceId();
                 auto probe = installer.emplaceSystem<Probe>(id);
                 if (!probe)
                 {
                     return cxx::unexpected(probe.error());
                 }
                 auto channel = installer.createHookChannel<SimulationBroadcastRoute, std::int32_t>(id, {1}, {1, 2, 2});
                 if (!channel)
                 {
                     return cxx::unexpected(channel.error());
                 }
                 (*probe)->channel = *channel;
                 auto producer = installer.bindHookChannelProducer(id, PrimarySimulationTask, **channel);
                 if (!producer)
                 {
                     return cxx::unexpected(producer.error());
                 }
                 (*probe)->producer = *producer;
                 assert(!(*channel)->begin(0).record(99));
                 assert(!(*channel)->beginOwner().record(99));
                 assert(!(*channel)->seal());
                 auto task = installer.addSystemTask<Probe>(
                     id,
                     [](Probe& probe) noexcept
                     {
                         auto writer = probe.producer.begin();
                         assert(writer.record(17));
                     }
                 );
                 if (!task)
                 {
                     return task;
                 }
                 return installer.addSystemHookTask<Probe>(
                     id,
                     {1},
                     [](Probe& probe, const HookInvocation& call) noexcept
                     {
                         assert(probe.channel->lane(0).size() == 1);
                         assert(probe.channel->lane(0)[0].payload == 17);
                         const auto owner = probe.channel->lane(1);
                         if (Probe::deliveries == 0)
                         {
                             assert(owner.empty());
                         }
                         else
                         {
                             assert(owner.size() == 1 && owner[0].payload == 23);
                         }
                         assert(!probe.producer.begin().record(99));
                         probe.channel->reset();
                         probe.channel->discard();
                         assert(probe.channel->lane(0).size() == 1);
                         {
                             auto writer = probe.channel->beginOwner(call);
                             assert(writer.record(23));
                         }
                         ++Probe::deliveries;
                     }
                 );
             }}
        ));
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem({1}, "probe", Probe::Description));
        assert(builder.addExecutionDependency(
            SimulationExecutionPoint::task({1}),
            SimulationExecutionPoint::hook({1}, {1})
        ));
        assert(builder.addChannelProducer({{1}, {1}, {1}, PrimarySimulationTask}));
        auto description = std::move(builder).build();
        assert(description);
        ecs::Registry registry;
        auto simulation =
            Simulation::create(registry, std::make_shared<SimulationDescription>(std::move(*description)), types);
        assert(simulation && simulation->seal());
        auto executor = task::TaskExecutor::create({2, 32});
        assert(executor);
        using namespace std::chrono_literals;
        for (std::uint64_t i = 1; i <= 3; ++i)
        {
            assert(simulation->execute(*executor, {i * 1ms, 1ms, i}));
        }
        assert(Probe::deliveries == 3);
    }
} // namespace

int main()
{
    standalone();
    composed();
    std::puts("Actual HookChannel and Simulation installer: storage, overflow, deferred owner, composed authority PASS"
    );
}
