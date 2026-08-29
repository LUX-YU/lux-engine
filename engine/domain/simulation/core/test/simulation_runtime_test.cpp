#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::ecs;

    inline constexpr SystemInstanceId FirstId{10U};
    inline constexpr SystemInstanceId SecondId{20U};
    inline constexpr SystemInstanceId CommandId{30U};
    inline constexpr SystemInstanceId ZeroId{40U};
    inline constexpr SystemInstanceId DuplicateId{50U};
    inline constexpr SystemInstanceId UndeclaredId{60U};
    inline constexpr SystemInstanceId ThrowingId{70U};

    struct Marker final
    {
        std::uint32_t value{};
    };

    struct TestContext final
    {
        std::vector<int> order;
        void* first_address{};
        std::size_t destroyed{};
        bool fail_command{};
    };

    struct FirstSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.test.simulation.first",
            .version = 1U
        };

        explicit FirstSystem(TestContext& context) noexcept : context_(&context)
        {
            context.first_address = this;
        }

        ~FirstSystem() noexcept
        {
            ++context_->destroyed;
        }

        bool run() noexcept
        {
            assert(context_->first_address == this);
            context_->order.push_back(1);
            return true;
        }

        TestContext* context_{};
    };

    struct SecondSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.test.simulation.second",
            .version = 1U
        };

        SecondSystem(TestContext& context, FirstSystem& first) noexcept
            : context_(&context), first_(&first)
        {
        }

        bool run() noexcept
        {
            assert(first_ != nullptr);
            context_->order.push_back(2);
            return true;
        }

        TestContext* context_{};
        FirstSystem* first_{};
    };

    struct CommandSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<ComponentWrite<Marker>>();
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.test.simulation.command",
            .version = 1U
        };

        explicit CommandSystem(TestContext& context) noexcept : context_(&context)
        {
        }

        bool run(EcsCommandWriter& writer) noexcept
        {
            const DeferredEntity entity = writer.create();
            if (!entity.valid() || !writer.emplace<Marker>(entity, Marker{42U}))
                return false;
            return !context_->fail_command;
        }

        TestContext* context_{};
    };

    struct ZeroTaskSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.test.simulation.zero",
            .version = 1U
        };
    };

    struct DuplicateTaskSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.test.simulation.duplicate",
            .version = 1U
        };

        void run() noexcept
        {
        }
    };

    struct UndeclaredSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.test.simulation.undeclared",
            .version = 1U
        };
    };

    struct ThrowingSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.test.simulation.throwing",
            .version = 1U
        };

        ThrowingSystem()
        {
            throw std::runtime_error("construction");
        }

        ~ThrowingSystem() noexcept = default;
    };

    lux::cxx::expected<void, SystemBuildFailure> installFirst(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto& context = builder.registry().ctx().get<TestContext>();
        auto system = builder.emplaceSystem<FirstSystem>(description.instanceId(), context);
        if (!system)
            return lux::cxx::unexpected(system.error());
        return builder.addSystemTask<FirstSystem>(
            description.instanceId(),
            [](FirstSystem& value) noexcept { return value.run(); }
        );
    }

    lux::cxx::expected<void, SystemBuildFailure> installSecond(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto* first = builder.findSystem<FirstSystem>(FirstId);
        if (first == nullptr)
        {
            return lux::cxx::unexpected(
                SystemBuildFailure{ESystemBuildError::INVALID_DESCRIPTION, description.instanceId()}
            );
        }
        auto& context = builder.registry().ctx().get<TestContext>();
        auto system = builder.emplaceSystem<SecondSystem>(description.instanceId(), context, *first);
        if (!system)
            return lux::cxx::unexpected(system.error());
        return builder.addSystemTask<SecondSystem>(
            description.instanceId(),
            [](SecondSystem& value) noexcept { return value.run(); }
        );
    }

    lux::cxx::expected<void, SystemBuildFailure> installCommand(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto& context = builder.registry().ctx().get<TestContext>();
        auto system = builder.emplaceSystem<CommandSystem>(description.instanceId(), context);
        if (!system)
            return lux::cxx::unexpected(system.error());
        return builder.addSystemCommandTask<CommandSystem>(
            description.instanceId(),
            EcsCommandProducerCapacity{4U, 256U},
            [](CommandSystem& value, EcsCommandWriter& writer) noexcept {
                return value.run(writer);
            }
        );
    }

    lux::cxx::expected<void, SystemBuildFailure> installZero(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto system = builder.emplaceSystem<ZeroTaskSystem>(description.instanceId());
        return system ? lux::cxx::expected<void, SystemBuildFailure>{}
                      : lux::cxx::unexpected(system.error());
    }

    lux::cxx::expected<void, SystemBuildFailure> installDuplicate(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto system = builder.emplaceSystem<DuplicateTaskSystem>(description.instanceId());
        if (!system)
            return lux::cxx::unexpected(system.error());
        auto first = builder.addSystemTask<DuplicateTaskSystem>(
            description.instanceId(),
            [](DuplicateTaskSystem& value) noexcept { value.run(); }
        );
        if (!first)
            return first;
        return builder.addSystemTask<DuplicateTaskSystem>(
            description.instanceId(),
            [](DuplicateTaskSystem& value) noexcept { value.run(); }
        );
    }

    lux::cxx::expected<void, SystemBuildFailure> installUndeclared(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        (void)builder.findSystem<FirstSystem>(FirstId);
        auto system = builder.emplaceSystem<UndeclaredSystem>(description.instanceId());
        return system ? lux::cxx::expected<void, SystemBuildFailure>{}
                      : lux::cxx::unexpected(system.error());
    }

    lux::cxx::expected<void, SystemBuildFailure> installThrowing(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto system = builder.emplaceSystem<ThrowingSystem>(description.instanceId());
        return system ? lux::cxx::expected<void, SystemBuildFailure>{}
                      : lux::cxx::unexpected(system.error());
    }

    [[nodiscard]] SystemRegistration registration(
        const SystemDescription& description,
        InstallSystemFn install,
        std::uint32_t version = 1U
    )
    {
        return SystemRegistration{systemTypeId(description.canonical_name), version, install};
    }

    [[nodiscard]] std::shared_ptr<const SimulationDescription> description(
        std::span<const std::pair<SystemInstanceId, const SystemDescription*>> systems,
        std::span<const std::pair<SystemInstanceId, SystemInstanceId>> dependencies = {}
    )
    {
        SimulationDescriptionBuilder builder;
        for (const auto& [instance, system] : systems)
            assert(builder.addSystem(instance, system->canonical_name, *system));
        for (const auto& [before, after] : dependencies)
            assert(builder.addDependency(before, after));
        auto built = std::move(builder).build();
        assert(built);
        return std::make_shared<SimulationDescription>(std::move(*built));
    }
}

int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::ecs;

    const std::array registrations{
        registration(FirstSystem::Description, &installFirst),
        registration(SecondSystem::Description, &installSecond),
        registration(CommandSystem::Description, &installCommand),
        registration(ZeroTaskSystem::Description, &installZero),
        registration(DuplicateTaskSystem::Description, &installDuplicate),
        registration(UndeclaredSystem::Description, &installUndeclared),
        registration(ThrowingSystem::Description, &installThrowing)
    };
    SystemRegistry registry_types;
    assert(registry_types.add(registrations));

    auto executor = lux::task::TaskExecutor::create({2U, 16U});
    assert(executor);

    {
        Registry registry;
        TestContext context;
        registry.ctx().emplace<TestContext>(std::move(context));
        const std::array systems{
            std::pair{SecondId, &SecondSystem::Description},
            std::pair{FirstId, &FirstSystem::Description}
        };
        const std::array dependencies{std::pair{FirstId, SecondId}};
        auto simulation = Simulation::create(
            registry,
            description(systems, dependencies),
            registry_types
        );
        assert(simulation);
        Simulation moved = std::move(*simulation);
        assert(moved.execute(*executor));
        const auto& result = registry.ctx().get<TestContext>();
        assert((result.order == std::vector<int>{1, 2}));
    }

    {
        Registry registry;
        registry.ctx().emplace<TestContext>();
        const std::array systems{std::pair{CommandId, &CommandSystem::Description}};
        auto simulation = Simulation::create(registry, description(systems), registry_types);
        assert(simulation);
        assert(simulation->execute(*executor));
        assert(registry.view<const Marker>().size() == 1U);
    }

    {
        Registry registry;
        auto& context = registry.ctx().emplace<TestContext>();
        context.fail_command = true;
        const std::array systems{std::pair{CommandId, &CommandSystem::Description}};
        auto simulation = Simulation::create(registry, description(systems), registry_types);
        assert(simulation);
        auto executed = simulation->execute(*executor);
        assert(!executed);
        assert(executed.error().code == ESimulationExecutionError::SYSTEM_TASK_FAILURE);
        assert(executed.error().system == CommandId);
        assert(registry.view<const Marker>().empty());
    }

    {
        Registry registry;
        registry.ctx().emplace<TestContext>();
        const std::array systems{
            std::pair{ZeroId, &ZeroTaskSystem::Description},
            std::pair{FirstId, &FirstSystem::Description}
        };
        const std::array dependencies{std::pair{ZeroId, FirstId}};
        auto simulation = Simulation::create(
            registry,
            description(systems, dependencies),
            registry_types
        );
        assert(!simulation);
        assert(simulation.error().code == ESystemBuildError::MISSING_PRIMARY_TASK);
    }

    {
        Registry registry;
        registry.ctx().emplace<TestContext>();
        const std::array systems{std::pair{DuplicateId, &DuplicateTaskSystem::Description}};
        auto simulation = Simulation::create(registry, description(systems), registry_types);
        assert(!simulation);
        assert(simulation.error().code == ESystemBuildError::DUPLICATE_PRIMARY_TASK);
    }

    {
        Registry registry;
        registry.ctx().emplace<TestContext>();
        const std::array systems{
            std::pair{FirstId, &FirstSystem::Description},
            std::pair{UndeclaredId, &UndeclaredSystem::Description}
        };
        auto simulation = Simulation::create(registry, description(systems), registry_types);
        assert(!simulation);
        assert(simulation.error().code == ESystemBuildError::UNDECLARED_CONSTRUCTOR_DEPENDENCY);
    }

    {
        Registry registry;
        auto& context = registry.ctx().emplace<TestContext>();
        const std::array systems{
            std::pair{FirstId, &FirstSystem::Description},
            std::pair{ThrowingId, &ThrowingSystem::Description}
        };
        const std::array dependencies{std::pair{FirstId, ThrowingId}};
        auto simulation = Simulation::create(
            registry,
            description(systems, dependencies),
            registry_types
        );
        assert(!simulation);
        assert(simulation.error().code == ESystemBuildError::CONSTRUCTION_FAILURE);
        assert(context.destroyed == 1U);
    }

    {
        Registry registry;
        registry.ctx().emplace<TestContext>();
        const std::array systems{std::pair{FirstId, &FirstSystem::Description}};
        SystemRegistry empty;
        auto unknown = Simulation::create(registry, description(systems), empty);
        assert(!unknown);
        assert(unknown.error().code == ESystemBuildError::UNKNOWN_SYSTEM_TYPE);

        SystemRegistry wrong_version;
        assert(wrong_version.add(registration(FirstSystem::Description, &installFirst, 2U)));
        auto mismatch = Simulation::create(registry, description(systems), wrong_version);
        assert(!mismatch);
        assert(mismatch.error().code == ESystemBuildError::VERSION_MISMATCH);
    }

    return 0;
}
