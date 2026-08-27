#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>
#include <lux/engine/simulation/detail/SimulationDescriptionFailureInjection.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

struct CollisionEvent final
{
    std::uint32_t body{};
};

namespace lux::semantic
{
    template <>
    struct TypeTraits<CollisionEvent> final
    {
        inline static constexpr std::string_view CanonicalName{
            "lux.event.Collision"};
        inline static constexpr std::uint8_t AbiKind{
            static_cast<std::uint8_t>(EAbiKind::STRUCT_REF)};
        inline static constexpr std::uint32_t Size{sizeof(CollisionEvent)};
        inline static constexpr std::uint32_t Alignment{alignof(CollisionEvent)};
    };
}

namespace
{
    using namespace lux::simulation;

    inline constexpr SystemInstanceId kPhysicsInstance{11U};
    inline constexpr SystemInstanceId kAnimationInstance{12U};
    inline constexpr HookPointId kPhysicsBefore{101U};
    inline constexpr HookPointId kPhysicsAfter{102U};
    inline constexpr EventPointId kCollisionEvent{201U};
    inline constexpr HookPointId kAnimationBefore{111U};
    inline constexpr HookPointId kAnimationAfter{112U};

    inline constexpr std::array kPhysicsCapabilities{
        std::string_view{"physics.simulate"},
        std::string_view{"physics.contacts"}};
    inline constexpr std::array kPhysicsHooks{
        makeHookPointSpec<void(const SimulationStepInfo&)>(
            kPhysicsBefore,
            "before"
        ),
        makeHookPointSpec<void(const SimulationStepInfo&)>(
            kPhysicsAfter,
            "after"
        )};
    inline constexpr std::array kPhysicsEvents{
        makeEventPointSpec<CollisionEvent>(
            kCollisionEvent,
            "collision",
            kPhysicsAfter,
            EEventRoute::ENTITY_TARGETED,
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
        makeHookPointSpec<void(const SimulationStepInfo&)>(
            kAnimationBefore,
            "before"
        ),
        makeHookPointSpec<void(const SimulationStepInfo&)>(
            kAnimationAfter,
            "after"
        )};
    inline constexpr std::array<EventPointSpec, 0U> kAnimationEvents{};
    inline constexpr SystemDescription kAnimationDescription{
        .canonical_name = "lux.animation",
        .version = 4U,
        .capabilities = kAnimationCapabilities,
        .hooks = kAnimationHooks,
        .events = kAnimationEvents};

    static_assert(validSystemDescription(kPhysicsDescription));
    static_assert(validSystemDescription(kAnimationDescription));

    void testSystemsAndDependencies()
    {
        SimulationDescriptionBuilder builder;
        constexpr std::array physics_config{std::byte{1U}};
        assert(builder.addSystem(
            kPhysicsInstance,
            "physics",
            kPhysicsDescription,
            physics_config
        ));
        assert(builder.addSystem(
            kAnimationInstance,
            "animation",
            kAnimationDescription
        ));
        assert(builder.addDependency(kPhysicsInstance, kAnimationInstance));

        auto built = std::move(builder).build();
        assert(built);
        assert(built->systemCount() == 2U);
        assert(built->systemAt(0U).instanceId() == kPhysicsInstance);
        assert(built->systemAt(1U).instanceId() == kAnimationInstance);
        assert(built->findSystem(kPhysicsInstance).instanceName() == "physics");
        assert(built->findSystem("physics").instanceId() == kPhysicsInstance);
        assert(built->findSystem(kPhysicsInstance).hasCapability(
            "physics.contacts"
        ));
        const auto collision = built->findEvent(
            kPhysicsInstance,
            kCollisionEvent
        );
        assert(collision);
        assert(collision.route() == EEventRoute::ENTITY_TARGETED);
        assert(collision.payloadType() == lux::semantic::typeId(
            "lux.event.Collision"
        ));
        assert(collision.payloadSchemaName() == "lux.event.Collision");
        assert(collision.dispatchHook().id() == kPhysicsAfter);
        const auto after = built->findHookPoint(
            kPhysicsInstance,
            kPhysicsAfter
        );
        assert(after && after.parameterCount() == 1U);
        assert(after.parameterAt(0U).pass ==
            lux::semantic::EValuePass::CONST_REF);
        assert(built->dependencyCount() == 1U);
        assert(built->dependencyAt(0U).before().instanceId() == kPhysicsInstance);
        assert(built->dependencyAt(0U).after().instanceId() == kAnimationInstance);

        SimulationDescriptionBuilder invalid;
        assert(invalid.addSystem(
            kPhysicsInstance,
            "physics",
            kPhysicsDescription,
            physics_config
        ));
        assert(!invalid.addSystem(
            kPhysicsInstance,
            "duplicate",
            kPhysicsDescription,
            physics_config
        ));
        assert(invalid.addSystem(
            kAnimationInstance,
            "animation",
            kAnimationDescription
        ));
        const auto missing = invalid.addDependency(
            SystemInstanceId{99U},
            kAnimationInstance
        );
        assert(!missing);
        assert(missing.error().code ==
            ESimulationDescriptionError::SYSTEM_NOT_FOUND);
        assert(!invalid.addDependency(kPhysicsInstance, kPhysicsInstance));
        assert(invalid.addDependency(kPhysicsInstance, kAnimationInstance));
        assert(!invalid.addDependency(kPhysicsInstance, kAnimationInstance));
        const auto cycle = invalid.addDependency(
            kAnimationInstance,
            kPhysicsInstance
        );
        assert(!cycle);
        assert(cycle.error().code ==
            ESimulationDescriptionError::DEPENDENCY_CYCLE);
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
    const std::array replacement{std::byte{9U}, std::byte{8U}};
    assert(validation.setData(gameplay, 4U, replacement));
    auto validated = std::move(validation).build();
    assert(validated && validated->dataCount() == 1U);
    assert(validated->findData(gameplay).version() == 4U);

    const auto canonical_a = buildDescription(false);
    const auto canonical_b = buildDescription(true);
    assert(canonical_a.dataCount() == canonical_b.dataCount());
    for (std::size_t index{}; index < canonical_a.dataCount(); ++index)
        assert(canonical_a.dataAt(index).schema() ==
            canonical_b.dataAt(index).schema());

    SimulationDescriptionBuilder recoverable;
    detail::failNextSimulationDescriptionOperationForTest(
        detail::ESimulationDescriptionFailurePoint::MUTATION_ALLOCATION
    );
    const auto mutation_failure = recoverable.addData(gameplay, 1U, {});
    assert(!mutation_failure);
    assert(mutation_failure.error().code ==
        ESimulationDescriptionError::ALLOCATION_FAILURE);
    assert(recoverable.addData(gameplay, 1U, {}));
    detail::failNextSimulationDescriptionOperationForTest(
        detail::ESimulationDescriptionFailurePoint::BUILD_ALLOCATION
    );
    assert(!std::move(recoverable).build());
    auto recovered = std::move(recoverable).build();
    assert(recovered && recovered->dataCount() == 1U);

    testSystemsAndDependencies();
    return 0;
}
