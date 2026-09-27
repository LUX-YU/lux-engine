#include <lux/engine/scene/detail/SceneDriver.hpp>
#include <lux/engine/scene/Clock.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/detail/SceneInstance.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemInstaller.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/TransformEvaluation.hpp>

#include <cassert>
#include <cmath>

namespace
{
    using namespace lux;
    using namespace lux::scene;
    namespace ecs = simulation::ecs;

    struct Notifications final
    {
        std::size_t count{};
        void changed(ecs::Registry&, ecs::Entity) noexcept
        {
            ++count;
        }
    };

    struct FailingRules final
    {
        inline static constexpr auto Access = simulation::makeSystemAccessSpec<>();
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        inline static constexpr simulation::SimulationSystemDescription Description{
            .type = {.canonical_name = "test.failing-rules", .version = 1, .supported_world_types = SupportedWorldTypes}
        };
        inline static unsigned calls{};
    };

    void checkAdoptedFailure()
    {
        using namespace std::chrono_literals;
        simulation::SimulationSystemRegistration registration{
            .type = system::systemTypeId(FailingRules::Description.type.canonical_name),
            .cpp_type = cxx::typeToken<FailingRules>(),
            .description = &FailingRules::Description,
            .access = FailingRules::Access.spec(),
            .install = +[](simulation::SimulationSystemInstaller& installer, simulation::SimulationSystemView input
                        ) noexcept -> cxx::expected<void, simulation::SimulationSystemBuildFailure> {
                auto rules = installer.emplaceSystem<FailingRules>(input.instanceId());
                if (!rules)
                    return cxx::unexpected(rules.error());
                return installer.addSystemTask<FailingRules>(input.instanceId(), [](auto&) noexcept {
                    ++FailingRules::calls;
                    return false;
                });
            }
        };
        simulation::SimulationSystemRegistry registrations;
        assert(registrations.add(std::span{&registration, 1U}));
        simulation::SimulationDescriptionBuilder builder;
        assert(builder.addSystem({1}, "failure", FailingRules::Description, {}));
        auto description = std::move(builder).build();
        assert(description);
        ecs::Registry registry;
        auto rules = simulation::Simulation::create(
            registry,
            std::make_shared<const simulation::SimulationDescription>(std::move(*description)),
            registrations
        );
        assert(rules && rules->seal());
        auto executor = task::TaskExecutor::create({0, 1024});
        assert(executor);
        assert(!rules->execute(*executor, {16ms, 8ms, 1}));
        assert(rules->time().step_index == 0 && FailingRules::calls == 0);
        const auto failed = rules->execute(*executor, {16ms, 16ms, 1});
        assert(!failed && failed.error().code == simulation::ESimulationExecutionError::SYSTEM_TASK_FAILURE);
        assert(failed.error().system.value == 1 && FailingRules::calls == 1);
        assert(rules->time().elapsed == 16ms && rules->time().step_index == 1);
    }

    struct CacheReader final
    {
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "test.cache-reader",
            .version = 1,
            .supported_world_types = SupportedWorldTypes
        };
        ecs::Registry& registry;
        std::size_t calls{};
    };

    SceneSystemRegistration cacheReaderRegistration()
    {
        return {
            .type = system::systemTypeId(CacheReader::Description.canonical_name),
            .cpp_type = cxx::typeToken<CacheReader>(),
            .description = &CacheReader::Description,
            .install = +[](SceneSystemInstaller& installer, SceneSystemDescription description
                        ) noexcept -> cxx::expected<void, SceneSystemBuildFailure> {
                auto reader = installer.emplaceSystem<CacheReader>(description.instanceId(), installer.registry());
                if (!reader)
                    return cxx::unexpected(reader.error());
                return installer.addStablePointTask<CacheReader>(description.instanceId(), [](auto& value) noexcept {
                    ++value.calls;
                    for (auto entity : value.registry.template view<const ecs::Transform3D>())
                    {
                        const auto fresh = ecs::computeWorldTransform3D(value.registry, entity);
                        assert(
                            fresh && value.registry.template get<ecs::WorldTransform3D>(entity).value.matrix().isApprox(
                                         fresh->matrix()
                                     )
                        );
                    }
                    return SceneStageResult{ESceneProgress::COMPLETE};
                });
            }
        };
    }

    void checkClocks()
    {
        using namespace std::chrono_literals;
        const auto now = FixedStepClock::TimePoint{1s};
        assert(!FixedStepClock::create(0ns));
        assert(!FixedStepClock::create(-1ns));
        VSimulationClock clocks;
        std::visit(
            [&](auto& clock) {
                static_assert(Clock<std::remove_cvref_t<decltype(clock)>>);
                const auto first = clock.sample(now);
                assert(first && **first == 16ms);
                // Sampling and DEFERRED admission do not change adopted time or deadline.
                assert(clock.sample(now) == first && clock.snapshot().step_index == 0 && !clock.deadline());
                clock.adopt(now, {16ms, 16ms, 1});
                assert(clock.sample(now) && !*clock.sample(now));
                assert(**clock.sample(now + 16ms) == 32ms);
                clock.rebase(now + 1h);
                assert(**clock.sample(now + 1h) == 32ms); // No wall-clock catch-up.
                clock.adopt(now + 1h, {32ms, 16ms, 2});   // Also used for an adopted, failed step.
                assert(clock.snapshot().elapsed == 32ms && clock.snapshot().step_index == 2);
                clock.adopt(now, {simulation::SimulationDuration::max(), {}, 2});
                const auto overflow = clock.sample(now + 16ms);
                assert(!overflow && overflow.error() == EClockError::TIME_OVERFLOW);
            },
            clocks
        );
    }

    void checkReactiveTransforms()
    {
        ecs::Registry registry;
        const auto parent = registry.create();
        const auto child = registry.create();
        registry.emplace<ecs::Transform3D>(parent).translation.x() = 10;
        registry.emplace<ecs::Transform3D>(child).translation.x() = 2;
        registry.emplace<ecs::Transform2D>(parent).translation.x() = 5;
        registry.emplace<ecs::Transform2D>(child).translation.x() = 1;
        registry.emplace<ecs::Parent>(child, parent);
        const auto immediate = ecs::computeWorldTransform3D(registry, child);
        assert(immediate && immediate->translation().x() == 12);
        assert(!registry.all_of<ecs::WorldTransform3D>(child));

        // Components precede observer installation; prepare must fold them in.
        TransformSystem transform(registry);
        assert(transform.prepare({64, 512, 65536}));
        SceneStageContext context;
        assert(transform.synchronize(context));
        assert(context.publication_needed);
        assert(registry.get<ecs::WorldTransform3D>(child).value.translation().x() == 12);
        assert(registry.get<ecs::WorldTransform2D>(child).value.translation().x() == 6);
        Notifications notices;
        entt::scoped_connection connection =
            registry.on_update<ecs::WorldTransform3D>().connect<&Notifications::changed>(notices);

        for (int index = 0; index < 100; ++index)
            registry.patch<ecs::Transform3D>(parent, [index](auto& value) { value.translation.x() = index; });
        assert(notices.count == 0); // Observers queue; they never walk descendants.
        const auto current = ecs::computeWorldTransform3D(registry, child);
        assert(current && current->translation().x() == 101);
        assert(registry.get<ecs::WorldTransform3D>(child).value.translation().x() == 12);
        context.publication_needed = false;
        assert(transform.synchronize(context));
        assert(notices.count == 2); // Overflow rebuild coalesces repeated changes.
        assert(registry.get<ecs::WorldTransform3D>(child).value.matrix().isApprox(current->matrix()));
        context.publication_needed = false;
        assert(transform.synchronize(context));
        assert(!context.publication_needed && notices.count == 2);

        registry.remove<ecs::Transform3D>(parent);
        assert(ecs::computeWorldTransform3D(registry, child)->translation().x() == 2);
        assert(transform.synchronize(context));
        assert(!registry.all_of<ecs::WorldTransform3D>(parent));
        assert(registry.get<ecs::WorldTransform3D>(child).value.translation().x() == 2);
        registry.destroy(parent);
        const auto invalid_parent = ecs::computeWorldTransform3D(registry, child);
        assert(!invalid_parent && invalid_parent.error() == ecs::ETransformEvaluationError::INVALID_PARENT);
        assert(transform.synchronize(context));
        assert(!registry.all_of<ecs::Parent>(child));
        assert(ecs::computeWorldTransform3D(registry, child));

        const auto other = registry.create();
        registry.emplace<ecs::Transform3D>(other);
        registry.emplace<ecs::Parent>(child, other);
        registry.emplace<ecs::Parent>(other, child);
        const auto cycle = ecs::computeWorldTransform3D(registry, child);
        assert(!cycle && cycle.error() == ecs::ETransformEvaluationError::HIERARCHY_CYCLE);
        assert(!transform.synchronize(context));
        const auto missing = registry.create();
        assert(
            ecs::computeWorldTransform3D(registry, missing).error() == ecs::ETransformEvaluationError::MISSING_TRANSFORM
        );
    }

    void checkPausedScene()
    {
        const auto transform = transformSystemRegistration();
        const auto config = makeTransformSystemConfiguration(64, {512, 65536});
        assert(config);
        SceneDescriptionBuilder builder;
        // Consumer installs first and declares no constructor dependency on Transform.
        const auto reader = cacheReaderRegistration();
        assert(builder.addSystem({2}, "reader", reader.type, 1, {}, 0));
        assert(builder.addSystem(
            {1},
            "transform",
            transform.type,
            1,
            transform.description->configuration_schema_name,
            1,
            *config
        ));
        auto description = std::move(builder).buildResolved();
        assert(description);
        simulation::SimulationSystemRegistry systems;
        ecs::ComponentSchemaSet components;
        const std::array registrations{reader, transform};
        auto scene = SceneInstance::create(
            {std::make_shared<const SceneDescription>(std::move(*description)),
             std::make_shared<const world::WorldDescription>(),
             std::make_shared<const simulation::SimulationDescription>(),
             components,
             systems,
             registrations,
             {}}
        );
        assert(scene);
        auto executor = task::TaskExecutor::create({0, 1024});
        assert(executor);
        SceneDriver driver(*executor);
        auto& registry = (*scene)->registry();
        const auto entity = registry.create();
        registry.emplace<ecs::Transform3D>(entity);
        assert(driver.maintain(**scene) == ESceneProgress::COMPLETE);
        assert(driver.publish(**scene) == ESceneProgress::COMPLETE);
        registry.patch<ecs::Transform3D>(entity, [](auto& value) { value.translation.x() = 7; });
        assert(driver.maintain(**scene) == ESceneProgress::COMPLETE);
        assert(driver.publish(**scene) == ESceneProgress::COMPLETE);
        assert(registry.get<ecs::WorldTransform3D>(entity).value.translation().x() == 7);
        assert((*scene)->simulation().time().step_index == 0 && (*scene)->atSafePoint());
        const auto& observed = *(*scene)->findSceneSystem<CacheReader>();
        assert(observed.calls == 2);
        for (unsigned turn{}; turn < 100; ++turn)
        {
            assert(driver.maintain(**scene) == ESceneProgress::COMPLETE);
            assert(driver.publish(**scene) == ESceneProgress::COMPLETE);
        }
        assert(observed.calls == 2); // Idle polling never recomputes stable consumers.
    }
}

void runTransformChecks()
{
    checkAdoptedFailure();
    checkClocks();
    checkReactiveTransforms();
    checkPausedScene();
}
