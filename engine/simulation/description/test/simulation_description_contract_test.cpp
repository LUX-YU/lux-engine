#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>
#include <lux/engine/simulation/detail/SimulationDescriptionFailureInjection.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::simulation;

    struct CollisionEvent final
    {
        std::uint32_t body{};
    };

    inline constexpr std::array kPhysicsCapabilities{
        std::string_view{"physics.simulate"},
        std::string_view{"physics.contacts"}};
    inline constexpr std::array kPhysicsHooks{
        makeSystemHookPoint<void(const SimulationStepInfo&)>("before"),
        makeSystemHookPoint<void(const SimulationStepInfo&)>("after")};
    inline constexpr std::array kPhysicsEvents{
        makeSystemEvent<CollisionEvent>(
            "collision",
            kPhysicsHooks[1],
            ESystemEventTarget::ENTITY_TARGETED,
            "lux.event.Collision",
            1U
        )};
    inline constexpr SystemDescription kPhysicsDescription{
        .canonical_name = "lux.physics",
        .version = 2U,
        .configuration_schema_name = "lux.physics.Config",
        .configuration_schema_version = 1U,
        .capabilities = kPhysicsCapabilities,
        .hooks = kPhysicsHooks,
        .events = kPhysicsEvents};

    inline constexpr std::array kAnimationCapabilities{
        std::string_view{"animation.evaluate"}};
    inline constexpr std::array kAnimationHooks{
        makeSystemHookPoint<void(const SimulationStepInfo&)>("before"),
        makeSystemHookPoint<void(const SimulationStepInfo&)>("after")};
    inline constexpr std::array kAnimationEvents{
        makeSystemEvent<void>(
            "finished",
            kAnimationHooks[1],
            ESystemEventTarget::GLOBAL,
            "must.be.ignored",
            42U
        )};
    inline constexpr SystemDescription kAnimationDescription{
        .canonical_name = "lux.animation",
        .version = 4U,
        .capabilities = kAnimationCapabilities,
        .hooks = kAnimationHooks,
        .events = kAnimationEvents};

    static_assert(validSystemDescription(kPhysicsDescription));
    static_assert(validSystemDescription(kAnimationDescription));
    static_assert(kAnimationEvents[0].payload_schema_name.empty());
    static_assert(kAnimationEvents[0].payload_schema_version == 0U);

    struct OrderedEvent final
    {
        std::size_t producer{};
        std::size_t local{};
    };

    void testTypedEventOrdering()
    {
        std::array<std::vector<OrderedEvent>, 3U> producers;
        for (auto& producer : producers)
            producer.reserve(4U);
        producers[2].push_back({2U, 0U});
        producers[0].push_back({0U, 0U});
        producers[1].push_back({1U, 0U});
        producers[0].push_back({0U, 1U});

        std::vector<OrderedEvent> received;
        received.reserve(4U);
        bool callback_ran{};
        for (std::size_t producer{}; producer < producers.size(); ++producer)
        {
            for (const auto event : producers[producer])
                received.push_back(event);
        }
        callback_ran = true;
        assert(callback_ran);
        assert(received.size() == 4U);
        assert(received[0].producer == 0U && received[0].local == 0U);
        assert(received[1].producer == 0U && received[1].local == 1U);
        assert(received[2].producer == 1U && received[2].local == 0U);
        assert(received[3].producer == 2U && received[3].local == 0U);
    }

    void testSystemsAndDependencies()
    {
        SimulationDescriptionBuilder builder;
        constexpr std::array physics_config{std::byte{1U}};
        assert(builder.addSystem(
            "physics.secondary",
            kPhysicsDescription,
            physics_config
        ));
        assert(builder.addSystem("animation", kAnimationDescription));
        assert(builder.addSystem(
            "physics.primary",
            kPhysicsDescription,
            physics_config
        ));
        assert(builder.addDependency(
            "physics.primary",
            "animation"
        ));
        std::array<std::uint8_t, 16U> script_id_bytes{};
        script_id_bytes[0] = 1U;
        ScriptMountDescription global_mount{
            lux::asset::AssetId{script_id_bytes},
            EScriptBindingSetMode::EXPLICIT,
            {{
                7U,
                lux::rdesc::EScriptBindingKind::HOOK,
                "lux.physics",
                "physics.primary",
                "after"
            }}};
        assert(builder.addGlobalScriptMount(global_mount));
        auto built = std::move(builder).build();
        assert(built);
        assert(built->systemCount() == 3U);
        assert(built->systemAt(0U).instanceName() == "animation");
        assert(built->systemAt(1U).instanceName() == "physics.primary");
        assert(built->systemAt(2U).instanceName() == "physics.secondary");
        assert(built->findSystem("physics.primary").hasCapability(
            "physics.contacts"
        ));
        assert(
            built->findEvent("physics.primary", "collision")
                .payloadSchemaName() == "lux.event.Collision"
        );
        assert(built->dependencyCount() == 1U);
        assert(built->dependencyAt(0U).before().instanceName() == "physics.primary");
        assert(built->dependencyAt(0U).after().instanceName() == "animation");
        assert(built->globalScriptMountCount() == 1U);
        assert(
            built->globalScriptMountAt(0U).bindingMode() ==
            EScriptBindingSetMode::EXPLICIT
        );
        assert(built->globalScriptMountAt(0U).bindingCount() == 1U);
        assert(built->globalScriptMountAt(0U).bindingAt(0U)->function == 7U);
        const auto after = built->findHookPoint("physics.primary", "after");
        assert(after);
        assert(after.cardinality() == ESystemHookCardinality::MULTI);
        assert(after.parameterCount() == 1U);
        assert(
            after.parameterAt(0U).pass ==
            lux::script::EScriptPassMode::CONST_REF
        );
        assert(
            built->findEvent("physics.primary", "collision").target() ==
            ESystemEventTarget::ENTITY_TARGETED
        );

        SimulationDescriptionBuilder invalid;
        assert(invalid.addSystem("physics", kPhysicsDescription, physics_config));
        assert(!invalid.addSystem("physics", kPhysicsDescription, physics_config));
        assert(invalid.addSystem("animation", kAnimationDescription));
        auto missing = invalid.addDependency(
            "missing",
            "animation"
        );
        assert(!missing);
        assert(missing.error().code == ESimulationDescriptionError::SYSTEM_NOT_FOUND);
        auto self = invalid.addDependency(
            "physics",
            "physics"
        );
        assert(!self);
        assert(self.error().code == ESimulationDescriptionError::INVALID_DEPENDENCY);
        assert(invalid.addDependency(
            "physics",
            "animation"
        ));
        assert(!invalid.addDependency(
            "physics",
            "animation"
        ));
        auto cycle = invalid.addDependency(
            "animation",
            "physics"
        );
        assert(!cycle);
        assert(cycle.error().code == ESimulationDescriptionError::DEPENDENCY_CYCLE);
    }

    [[nodiscard]] SimulationDescription buildDescription(bool reverse)
    {
        SimulationDescriptionBuilder builder;
        const auto gameplay = simulationDataSchemaId("test.gameplay");
        const auto spatial = simulationDataSchemaId("test.spatial3d");
        const std::array gameplay_payload{std::byte{1U}, std::byte{2U}};
        const std::array spatial_payload{std::byte{3U}};
        if (reverse)
        {
            assert(builder.addData(spatial, 7U, spatial_payload));
            assert(builder.addData(gameplay, 2U, gameplay_payload));
        }
        else
        {
            assert(builder.addData(gameplay, 2U, gameplay_payload));
            assert(builder.addData(spatial, 7U, spatial_payload));
        }
        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }

    void assertEquivalent(
        const SimulationDescription& left,
        const SimulationDescription& right
    )
    {
        assert(left.dataCount() == right.dataCount());
        assert(left.payloadBytes() == right.payloadBytes());
        for (std::size_t index{}; index < left.dataCount(); ++index)
        {
            const auto left_data = left.dataAt(index);
            const auto right_data = right.dataAt(index);
            assert(left_data.schema() == right_data.schema());
            assert(left_data.version() == right_data.version());
            assert(left_data.payload().size() == right_data.payload().size());
            for (std::size_t byte{}; byte < left_data.payload().size(); ++byte)
                assert(left_data.payload()[byte] == right_data.payload()[byte]);
        }
    }
}

int main()
{
    using namespace lux::simulation;

    SimulationDescriptionBuilder validation;
    assert(!validation.addData({}, 1U, {}));
    const auto gameplay = simulationDataSchemaId("test.gameplay");
    assert(!validation.addData(gameplay, 0U, {}));
    assert(validation.addData(gameplay, 1U, {}));
    assert(!validation.addData(gameplay, 1U, {}));
    assert(!validation.eraseData(simulationDataSchemaId("test.missing")));

    const std::array replacement{std::byte{9U}, std::byte{8U}};
    assert(validation.setData(gameplay, 4U, replacement));
    auto validated = std::move(validation).build();
    assert(validated);
    assert(validated->dataCount() == 1U);
    assert(validated->findData(gameplay));
    assert(validated->findData(gameplay).version() == 4U);
    assert(validated->findData(gameplay).payload().size() == 2U);
    assert(validated->retainedBytes() >= validated->payloadBytes());
    assert(!validated->findData(simulationDataSchemaId("test.missing")));
    assert(!validated->dataAt(1U));

    auto canonical_a = buildDescription(false);
    auto canonical_b = buildDescription(true);
    assertEquivalent(canonical_a, canonical_b);

    SimulationDescriptionBuilder recoverable;
    detail::failNextSimulationDescriptionOperationForTest(
        detail::ESimulationDescriptionFailurePoint::MUTATION_ALLOCATION
    );
    const auto mutation_failure = recoverable.addData(gameplay, 1U, {});
    assert(!mutation_failure);
    assert(
        mutation_failure.error().code ==
        ESimulationDescriptionError::ALLOCATION_FAILURE
    );
    assert(recoverable.addData(gameplay, 1U, {}));

    detail::failNextSimulationDescriptionOperationForTest(
        detail::ESimulationDescriptionFailurePoint::BUILD_ALLOCATION
    );
    const auto build_failure = std::move(recoverable).build();
    assert(!build_failure);
    assert(
        build_failure.error().code ==
        ESimulationDescriptionError::ALLOCATION_FAILURE
    );
    detail::failNextSimulationDescriptionOperationForTest(
        detail::ESimulationDescriptionFailurePoint::BUILD_SIZE_OVERFLOW
    );
    const auto overflow_failure = std::move(recoverable).build();
    assert(!overflow_failure);
    assert(
        overflow_failure.error().code ==
        ESimulationDescriptionError::SIZE_OVERFLOW
    );
    auto recovered = std::move(recoverable).build();
    assert(recovered);
    assert(recovered->dataCount() == 1U);

    SimulationDescriptionBuilder erase;
    assert(erase.addData(gameplay, 1U, {}));
    assert(erase.eraseData(gameplay));
    auto empty = std::move(erase).build();
    assert(empty);
    assert(empty->empty());

    testSystemsAndDependencies();
    testTypedEventOrdering();

    return 0;
}
