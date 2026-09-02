#include "TestAbility.hpp"
#include "TestAbility.ability.generated.hpp"

#include <lux/engine/function/script/ScriptAbility.hpp>
#include <lux/engine/description/Script.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <memory>
#include <utility>

namespace
{
    template <class Api>
    concept HasImmediateBeginOperation = requires(Api api)
    {
        api.beginOperation(std::uint64_t{});
    };

    struct TestProvider final
    {
        std::int32_t value{};
        std::size_t calls{};

        std::int32_t readValue(std::int32_t input) noexcept
        {
            ++calls;
            return value + input;
        }

        void setValue(std::int32_t new_value) noexcept
        {
            ++calls;
            value = new_value;
        }

        std::uint64_t identity(std::uint64_t value) noexcept
        {
            return value;
        }

        const std::int32_t& borrowedValue() noexcept
        {
            return value;
        }

        lux::script::ScriptAbilityStartResult beginOperation(
            std::uint64_t,
            lux::script::ScriptAbilityCompletion<std::uint64_t>
        ) noexcept
        {
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{71});
        }
    };

    struct ProviderLifetime final
    {
        std::size_t constructed{};
        std::size_t destroyed{};
        std::size_t calls{};
    };

    struct OwnedTestProvider final
    {
        inline static constexpr auto Access = lux::simulation::makeSystemAccessSpec<>();
        inline static constexpr lux::simulation::SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.test.script.ability.provider", .version = 1U}
        };

        explicit OwnedTestProvider(ProviderLifetime& lifetime) noexcept : lifetime_(&lifetime)
        {
            ++lifetime_->constructed;
        }

        ~OwnedTestProvider() noexcept
        {
            ++lifetime_->destroyed;
        }

        std::int32_t readValue(std::int32_t input) noexcept
        {
            ++lifetime_->calls;
            return value_ + input;
        }

        void setValue(std::int32_t value) noexcept
        {
            ++lifetime_->calls;
            value_ = value;
        }

        std::uint64_t identity(std::uint64_t value) noexcept
        {
            return value;
        }

        const std::int32_t& borrowedValue() noexcept
        {
            return value_;
        }

        lux::script::ScriptAbilityStartResult beginOperation(
            std::uint64_t,
            lux::script::ScriptAbilityCompletion<std::uint64_t>
        ) noexcept
        {
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{72});
        }

        ProviderLifetime* lifetime_{};
        std::int32_t value_{10};
    };

    struct SecondOwnedTestProvider final
    {
        inline static constexpr auto Access = lux::simulation::makeSystemAccessSpec<>();
        inline static constexpr lux::simulation::SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.test.script.ability.second_provider", .version = 1U}
        };

        explicit SecondOwnedTestProvider(ProviderLifetime& lifetime) noexcept : lifetime_(&lifetime)
        {
            ++lifetime_->constructed;
        }

        ~SecondOwnedTestProvider() noexcept
        {
            ++lifetime_->destroyed;
        }

        std::int32_t readValue(std::int32_t input) noexcept
        {
            ++lifetime_->calls;
            return input;
        }

        void setValue(std::int32_t) noexcept
        {
            ++lifetime_->calls;
        }

        std::uint64_t identity(std::uint64_t value) noexcept
        {
            return value;
        }

        const std::int32_t& borrowedValue() noexcept
        {
            return borrowed_value_;
        }

        lux::script::ScriptAbilityStartResult beginOperation(
            std::uint64_t,
            lux::script::ScriptAbilityCompletion<std::uint64_t>
        ) noexcept
        {
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{73});
        }

        ProviderLifetime* lifetime_{};
        std::int32_t borrowed_value_{};
    };

    inline constexpr lux::system::SystemInstanceId FirstProviderId{1U};
    inline constexpr lux::system::SystemInstanceId SecondProviderId{2U};

    template <class Provider>
    lux::cxx::expected<void, lux::simulation::SimulationSystemBuildFailure> installProvider(
        lux::simulation::SimulationBuilder& builder,
        lux::simulation::SimulationSystemView description
    ) noexcept
    {
        auto& lifetime = builder.registry().ctx().get<ProviderLifetime>();
        if constexpr (std::same_as<Provider, OwnedTestProvider>)
            assert(builder.scriptApiCapabilities().empty());
        else
            assert(builder.scriptApiCapabilities().size() == 1U);
        auto provider = builder.emplaceSystem<Provider>(description.instanceId(), lifetime);
        if (!provider)
            return lux::cxx::unexpected(provider.error());
        return builder.publishScriptAbility(
            description.instanceId(),
            lux::script::bindScriptAbility<lux::simulation::test::TestAbility>(**provider)
        );
    }

    template <class Provider>
    lux::simulation::SimulationSystemRegistration registration(
        const lux::simulation::SimulationSystemDescription& description
    )
    {
        return {
            .type = lux::system::systemTypeId(description.type.canonical_name),
            .cpp_type = lux::cxx::typeToken<Provider>(),
            .description = &description,
            .access = Provider::Access.spec(),
            .configuration = lux::serialization::noPortableValueCodec(),
            .install = &installProvider<Provider>
        };
    }

    std::shared_ptr<const lux::simulation::SimulationDescription> simulationDescription(
        std::span<const std::pair<lux::system::SystemInstanceId, const lux::simulation::SimulationSystemDescription*>>
            systems
    )
    {
        lux::simulation::SimulationDescriptionBuilder builder;
        for (const auto& [instance, system] : systems)
            assert(builder.addSystem(instance, system->type.canonical_name, *system));
        auto built = std::move(builder).build();
        assert(built);
        return std::make_shared<lux::simulation::SimulationDescription>(std::move(*built));
    }
}

int main()
{
    using lux::script::ScriptAbilityCpp;
    using lux::script::ScriptAbilityTraits;
    using lux::script::bindScriptAbility;
    using lux::simulation::test::TestAbility;
    using lux::simulation::test::TestStatelessAbility;

    static_assert(ScriptAbilityTraits<TestAbility>::Description.schema_hash != 0U);
    static_assert(ScriptAbilityTraits<TestAbility>::Description.methods.size() == 5U);
    static_assert(ScriptAbilityTraits<TestStatelessAbility>::Description.methods.size() == 1U);
    static_assert(!HasImmediateBeginOperation<ScriptAbilityCpp<TestAbility>>);
    static_assert(ScriptAbilityTraits<TestAbility>::Description.schema_version == 1U);
    constexpr auto ReversedMethods = []() consteval {
        auto methods = ScriptAbilityTraits<TestAbility>::Methods;
        for (std::size_t index{}; index < methods.size() / 2U; ++index)
            std::swap(methods[index], methods[methods.size() - index - 1U]);
        return methods;
    }();
    static_assert(
        lux::script::scriptAbilitySchemaHash(
            ScriptAbilityTraits<TestAbility>::Contract,
            ScriptAbilityTraits<TestAbility>::Receiver,
            ReversedMethods,
            ScriptAbilityTraits<TestAbility>::Description.schema_version
        ) == ScriptAbilityTraits<TestAbility>::Description.schema_hash
    );
    constexpr auto ChangedMethods = []() consteval {
        auto methods = ScriptAbilityTraits<TestAbility>::Methods;
        methods[0].kind = lux::script::EScriptApiMethodKind::COMMAND;
        return methods;
    }();
    static_assert(
        lux::script::scriptAbilitySchemaHash(
            ScriptAbilityTraits<TestAbility>::Contract,
            ScriptAbilityTraits<TestAbility>::Receiver,
            ChangedMethods,
            ScriptAbilityTraits<TestAbility>::Description.schema_version
        ) != ScriptAbilityTraits<TestAbility>::Description.schema_hash
    );
    static constexpr std::array ChangedParameters{
        lux::script::ScriptAbilityParameterDescription{
            "input",
            lux::script::makeScriptAbilityValue<std::uint32_t>(
                lux::script::EScriptAbilityValueLifetime::OWNED_VALUE
            )
        }
    };
    constexpr auto ChangedParameterMethods = []() consteval {
        auto methods = ScriptAbilityTraits<TestAbility>::Methods;
        methods[0].parameters = ChangedParameters;
        return methods;
    }();
    static_assert(
        lux::script::scriptAbilitySchemaHash(
            ScriptAbilityTraits<TestAbility>::Contract,
            ScriptAbilityTraits<TestAbility>::Receiver,
            ChangedParameterMethods,
            ScriptAbilityTraits<TestAbility>::Description.schema_version
        ) != ScriptAbilityTraits<TestAbility>::Description.schema_hash
    );
    static constexpr std::array ChangedResults{
        lux::script::makeScriptAbilityValue<std::int32_t>(
            lux::script::EScriptAbilityValueLifetime::STABLE_ID
        )
    };
    constexpr auto ChangedLifetimeMethods = []() consteval {
        auto methods = ScriptAbilityTraits<TestAbility>::Methods;
        methods[0].results = ChangedResults;
        return methods;
    }();
    static_assert(
        lux::script::scriptAbilitySchemaHash(
            ScriptAbilityTraits<TestAbility>::Contract,
            ScriptAbilityTraits<TestAbility>::Receiver,
            ChangedLifetimeMethods,
            ScriptAbilityTraits<TestAbility>::Description.schema_version
        ) != ScriptAbilityTraits<TestAbility>::Description.schema_hash
    );
    static constexpr std::array ChangedResultTypes{
        lux::script::makeScriptAbilityValue<std::uint32_t>(
            lux::script::EScriptAbilityValueLifetime::OWNED_VALUE
        )
    };
    constexpr auto ChangedResultTypeMethods = []() consteval {
        auto methods = ScriptAbilityTraits<TestAbility>::Methods;
        methods[0].results = ChangedResultTypes;
        return methods;
    }();
    static_assert(
        lux::script::scriptAbilitySchemaHash(
            ScriptAbilityTraits<TestAbility>::Contract,
            ScriptAbilityTraits<TestAbility>::Receiver,
            ChangedResultTypeMethods,
            ScriptAbilityTraits<TestAbility>::Description.schema_version
        ) != ScriptAbilityTraits<TestAbility>::Description.schema_hash
    );
    static constexpr std::array RenamedParameters{
        lux::script::ScriptAbilityParameterDescription{
            "renamed-input",
            ScriptAbilityTraits<TestAbility>::Methods[0].parameters[0].value
        }
    };
    constexpr auto RenamedParameterMethods = []() consteval {
        auto methods = ScriptAbilityTraits<TestAbility>::Methods;
        methods[0].parameters = RenamedParameters;
        return methods;
    }();
    static_assert(
        lux::script::scriptAbilitySchemaHash(
            ScriptAbilityTraits<TestAbility>::Contract,
            ScriptAbilityTraits<TestAbility>::Receiver,
            RenamedParameterMethods,
            ScriptAbilityTraits<TestAbility>::Description.schema_version
        ) == ScriptAbilityTraits<TestAbility>::Description.schema_hash
    );
    constexpr auto ChangedDisplayMethods = []() consteval {
        auto methods = ScriptAbilityTraits<TestAbility>::Methods;
        methods[0].display_name = "Renamed Display";
        return methods;
    }();
    static_assert(
        lux::script::scriptAbilitySchemaHash(
            ScriptAbilityTraits<TestAbility>::Contract,
            ScriptAbilityTraits<TestAbility>::Receiver,
            ChangedDisplayMethods,
            ScriptAbilityTraits<TestAbility>::Description.schema_version
        ) == ScriptAbilityTraits<TestAbility>::Description.schema_hash
    );
    static_assert(
        lux::script::scriptAbilitySchemaHash(
            ScriptAbilityTraits<TestAbility>::Contract,
            lux::script::EScriptAbilityReceiverKind::NONE,
            ScriptAbilityTraits<TestAbility>::Methods,
            ScriptAbilityTraits<TestAbility>::Description.schema_version
        ) != ScriptAbilityTraits<TestAbility>::Description.schema_hash
    );
    static_assert(
        lux::script::scriptAbilitySchemaHash(
            ScriptAbilityTraits<TestAbility>::Contract,
            ScriptAbilityTraits<TestAbility>::Receiver,
            ScriptAbilityTraits<TestAbility>::Methods,
            ScriptAbilityTraits<TestAbility>::Description.schema_version + 1U
        ) != ScriptAbilityTraits<TestAbility>::Description.schema_hash
    );

    TestProvider provider{7};
    const auto binding = bindScriptAbility<TestAbility>(provider);
    assert(binding.valid());
    assert(binding.context == &provider);

    auto api = ScriptAbilityCpp<TestAbility>::create(binding);
    assert(api);
    assert(api->readValue(5) == 12);
    api->setValue(41);
    assert(provider.value == 41);
    assert(provider.calls == 2U);
    assert(api->identity(0x51U) == 0x51U);
    assert(&api->borrowedValue() == &provider.value);
    auto starter = lux::script::ScriptAbilityStarter<TestAbility>::create(binding);
    assert(starter);
    auto rejected_start = starter->beginOperation(0x52U, {});
    assert(!rejected_start && rejected_start.error().status == 71);

    const auto& methods = ScriptAbilityTraits<TestAbility>::Description.methods;
    assert(methods[2].parameters.front().value.lifetime == lux::script::EScriptAbilityValueLifetime::STABLE_ID);
    assert(methods[2].results.front().lifetime == lux::script::EScriptAbilityValueLifetime::STABLE_ID);
    assert(methods[3].results.front().lifetime == lux::script::EScriptAbilityValueLifetime::BORROWED_STEP);
    assert(methods[4].kind == lux::script::EScriptApiMethodKind::ASYNC_OPERATION);
    assert(methods[4].results.front().lifetime == lux::script::EScriptAbilityValueLifetime::AWAITABLE);

    const lux::rdesc::ScriptApiRequirement requirement{
        lux::script::ScriptApiContractId{ScriptAbilityTraits<TestAbility>::Description.id.name()},
        ScriptAbilityTraits<TestAbility>::Description.schema_hash
    };
    assert(requirement.contract.view() == ScriptAbilityTraits<TestAbility>::Description.id);
    assert(requirement.expected_schema_hash == ScriptAbilityTraits<TestAbility>::Description.schema_hash);

    const auto stateless_binding = bindScriptAbility<TestStatelessAbility>();
    assert(stateless_binding.valid());
    assert(stateless_binding.context == nullptr);
    auto stateless = ScriptAbilityCpp<TestStatelessAbility>::create(stateless_binding);
    assert(stateless);
    assert(stateless->increment(4) == 5);

    lux::simulation::SimulationSystemRegistry system_types;
    const std::array provider_registrations{
        registration<OwnedTestProvider>(OwnedTestProvider::Description),
        registration<SecondOwnedTestProvider>(SecondOwnedTestProvider::Description)
    };
    assert(system_types.add(provider_registrations));

    {
        lux::simulation::ecs::Registry registry;
        auto& lifetime = registry.ctx().emplace<ProviderLifetime>();
        {
            const std::array systems{std::pair{FirstProviderId, &OwnedTestProvider::Description}};
            auto simulation = lux::simulation::Simulation::create(
                registry,
                simulationDescription(systems),
                system_types
            );
            assert(simulation);
            assert(lifetime.constructed == 1U);
            assert(lifetime.destroyed == 0U);
            const auto capabilities = simulation->scriptApiCapabilities();
            assert(capabilities.size() == 1U);
            const lux::script::ScriptAbilityBinding owned_binding{
                &ScriptAbilityTraits<TestAbility>::Description,
                capabilities.front().context,
                capabilities.front().dispatch
            };
            auto owned_api = ScriptAbilityCpp<TestAbility>::create(owned_binding);
            assert(owned_api);
            assert(owned_api->readValue(2) == 12);
        }
        assert(lifetime.destroyed == 1U);
    }

    {
        lux::simulation::ecs::Registry registry;
        auto& lifetime = registry.ctx().emplace<ProviderLifetime>();
        const std::array systems{
            std::pair{FirstProviderId, &OwnedTestProvider::Description},
            std::pair{SecondProviderId, &SecondOwnedTestProvider::Description}
        };
        auto simulation = lux::simulation::Simulation::create(
            registry,
            simulationDescription(systems),
            system_types
        );
        assert(!simulation);
        assert(
            simulation.error().code ==
            lux::simulation::ESimulationSystemBuildError::SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER
        );
        assert(simulation.error().system == SecondProviderId);
        assert(simulation.error().related == FirstProviderId);
        assert(lifetime.constructed == 2U);
        assert(lifetime.destroyed == 2U);
    }
    return 0;
}
