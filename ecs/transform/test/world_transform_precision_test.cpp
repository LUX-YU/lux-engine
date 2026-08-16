#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/SpatialTransformMath.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/transform/systems/Transform2DSystem.hpp>
#include <lux/engine/ecs/transform/systems/Transform3DSystem.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace
{
    [[nodiscard]] bool near(double left, double right) noexcept
    {
        return std::abs(left - right) < 1.0e-6;
    }

    class ParentBatchSystem final : public lux::ecs::ISystem
    {
    public:
        struct SetParentCommand final
        {
            using Producer = ParentBatchSystem;

            entt::entity child{entt::null};
            entt::entity parent{entt::null};

            [[nodiscard]] std::size_t registryPublicationBytes()
                const noexcept
            {
                return lux::ecs::ecsCommandSparsePublicationBytes(3u);
            }
            void prepareRegistryPublication(
                lux::meta::EntityRegistry& registry) const noexcept
            {
                constexpr std::size_t kMaximumFixtureEdges = 1024u;
                auto& parents =
                    registry.storage<lux::ecs::ParentComponent>();
                if (!lux::ecs::reserveStorageCapacity(
                        parents,
                        std::max(
                            parents.capacity(), kMaximumFixtureEdges)))
                {
                    std::abort();
                }
                auto& hierarchy = lux::ecs::ensureHierarchyIndex(registry);
                if (!hierarchy.reserveForAdditionalEdges(
                        kMaximumFixtureEdges))
                {
                    std::abort();
                }
            }

            void apply(
                lux::meta::EntityRegistry& registry,
                ParentBatchSystem&) const noexcept
            {
                assert(lux::ecs::setParent(registry, child, parent));
            }
        };

        void onAdded(const lux::ecs::SystemSetupContext& setup) override
        {
            commands_ = setup.commands();
        }

        void queue(entt::entity child, entt::entity parent)
        {
            assert(commands_.push(SetParentCommand{child, parent}));
        }

        void update(const lux::ecs::SystemUpdateContext&) override {}

    private:
        lux::ecs::EcsCommandWriter commands_;
    };
}

int main()
{
    using namespace lux::ecs;

    Transform3DComponent root_local;
    root_local.position = {1.0e12, -1.0e12, 5.0e11};
    root_local.rotation = Eigen::AngleAxisf(
        0.5f * 3.1415926535f,
        Eigen::Vector3f::UnitZ()
    );
    root_local.scale = {2.0f, 3.0f, 4.0f};
    const auto root = Transform3DPolicy::composeRoot(root_local);
    assert(root.position == root_local.position);

    ResolvedTransform3DComponent parent;
    parent.position = root.position;
    parent.linear = root.linear;
    Transform3DComponent child_local;
    child_local.position = {1.0, 2.0, 3.0};
    const auto child = Transform3DPolicy::composeChild(child_local, parent);
    assert(near(child.position.x, root.position.x - 6.0));
    assert(near(child.position.y, root.position.y + 2.0));
    assert(near(child.position.z, root.position.z + 12.0));

    const auto relative = relativePosition(
        child.position,
        root.position,
        100.0f
    );
    assert(relative);
    assert(near(relative->x(), -6.0));
    assert(near(relative->y(), 2.0));
    assert(near(relative->z(), 12.0));

    Transform2DComponent root_2d_local;
    root_2d_local.position = {-4.0e11, 3.0e11};
    const auto root_2d = Transform2DPolicy::composeRoot(root_2d_local);
    ResolvedTransform2DComponent parent_2d;
    parent_2d.position = root_2d.position;
    parent_2d.linear = root_2d.linear;
    Transform2DComponent child_2d_local;
    child_2d_local.position = {-1.0, 1.0};
    const auto child_2d = Transform2DPolicy::composeChild(
        child_2d_local,
        parent_2d
    );
    assert(near(child_2d.position.x, root_2d.position.x - 1.0));
    assert(near(child_2d.position.y, root_2d.position.y + 1.0));

    World world;
    auto& registry = world.registry();
    auto& hierarchy = ensureHierarchyIndex(registry);
    const auto root_entity = registry.create();
    const auto child_entity = registry.create();
    const auto leaf_entity = registry.create();
    registry.emplace<Transform3DComponent>(root_entity, root_local);
    registry.emplace<Transform3DComponent>(child_entity, child_local);
    registry.emplace<Transform3DComponent>(leaf_entity, Transform3DComponent{});
    assert(setParent(registry, child_entity, root_entity));
    assert(setParent(registry, leaf_entity, child_entity));
    assert(hierarchy.topologyDirty());
    const auto rebuild_before = hierarchy.rebuildCount();
    assert(hierarchy.refresh());
    assert(hierarchy.rebuildCount() == rebuild_before + 1u);
    assert(!hierarchy.refresh());
    const auto root_range = hierarchy.subtreeRange(root_entity);
    assert(root_range && root_range->count == 3u);

    Schedule schedule{world};
    auto system = std::make_unique<Transform3DSystem>();
    auto* system_observer = system.get();
    const auto installed = schedule.addSystem(std::move(system));
    assert(installed);
    assert(schedule.compile().valid());
    schedule.tick(0.0f);
    assert(system_observer->lastVisitedCount() == 3u);
    schedule.tick(0.0f);
    schedule.tick(0.0f);
    assert(system_observer->lastVisitedCount() == 0u);

    registry.patch<Transform3DComponent>(leaf_entity, [](auto& transform)
    {
        transform.position.x += 0.001;
    });
    schedule.tick(0.0f);
    assert(system_observer->lastVisitedCount() == 1u);

    registry.patch<Transform3DComponent>(child_entity, [](auto& transform)
    {
        transform.position.y += 0.001;
    });
    schedule.tick(0.0f);
    assert(system_observer->lastVisitedCount() == 2u);

    // A hierarchy parent is not required to carry a Transform. Its lifetime
    // must nevertheless invalidate topology so the surviving child becomes a
    // root on the next barrier/index refresh.
    const auto transformless_parent = registry.create();
    const auto detached_child = registry.create();
    registry.emplace<Transform3DComponent>(
        detached_child,
        Transform3DComponent{});
    assert(setParent(registry, detached_child, transformless_parent));
    assert(hierarchy.refresh());
    const auto parent_destroy_rebuild = hierarchy.rebuildCount();
    registry.destroy(transformless_parent);
    assert(hierarchy.topologyDirty());
    assert(hierarchy.refresh());
    assert(hierarchy.rebuildCount() == parent_destroy_rebuild + 1u);
    assert(!hierarchy.refresh());
    assert(hierarchyRoot(registry, detached_child) == detached_child);
    const auto detached_range = hierarchy.subtreeRange(detached_child);
    assert(detached_range && detached_range->count == 1u);

    // Dynamic contribution removal must detach both the reactive change set
    // and the stateless derived-component observers. Re-adding the system
    // folds facts created or removed during the gap before resolving them.
    const auto removed_during_gap = registry.create();
    registry.emplace<Transform3DComponent>(
        removed_during_gap,
        Transform3DComponent{}
    );
    schedule.tick(0.0f);
    assert(registry.all_of<ResolvedTransform3DComponent>(removed_during_gap));
    assert(schedule.removeSystem(*installed).has_value());

    const auto added_during_gap = registry.create();
    registry.emplace<Transform3DComponent>(
        added_during_gap,
        Transform3DComponent{}
    );
    registry.remove<Transform3DComponent>(removed_during_gap);
    assert(!registry.all_of<ResolvedTransform3DComponent>(added_during_gap));
    assert(registry.all_of<ResolvedTransform3DComponent>(removed_during_gap));

    const auto reinstalled = schedule.addSystem(
        std::make_unique<Transform3DSystem>()
    );
    assert(reinstalled);
    assert(schedule.compile().valid());
    schedule.tick(0.0f);
    assert(registry.all_of<ResolvedTransform3DComponent>(added_during_gap));
    assert(!registry.all_of<ResolvedTransform3DComponent>(removed_during_gap));

    registry.remove<Transform3DComponent>(added_during_gap);
    assert(!registry.all_of<ResolvedTransform3DComponent>(added_during_gap));
    registry.emplace<Transform3DComponent>(
        added_during_gap,
        Transform3DComponent{}
    );
    assert(registry.all_of<ResolvedTransform3DComponent>(added_during_gap));

    // A command barrier containing many hierarchy edges owns exactly one
    // topology rebuild. Individual ParentComponent signals only mark dirty.
    {
        World hierarchy_world;
        auto& hierarchy_registry = hierarchy_world.registry();
        auto& hierarchy_index = ensureHierarchyIndex(hierarchy_registry);
        Schedule hierarchy_schedule{hierarchy_world};
        auto producer = std::make_unique<ParentBatchSystem>();
        auto* producer_observer = producer.get();
        assert(hierarchy_schedule.addSystem(std::move(producer)));
        assert(hierarchy_schedule.compile().valid());

        constexpr std::size_t kChildren = 1024u;
        const auto parent = hierarchy_registry.create();
        for (std::size_t index = 0u; index < kChildren; ++index)
        {
            const auto entity = hierarchy_registry.create();
            producer_observer->queue(entity, parent);
        }
        const auto before = hierarchy_index.rebuildCount();
        assert(hierarchy_schedule.prepareCommandBarrier());
        hierarchy_schedule.applyCommandBarrier();
        assert(hierarchy_index.rebuildCount() == before + 1u);
        const auto range = hierarchy_index.subtreeRange(parent);
        assert(range && range->count == kChildren + 1u);
    }

    // A large flat registry has no hidden per-frame hierarchy scan. The first
    // tick resolves newly inserted facts; subsequent unchanged ticks visit 0.
    {
        World flat_world;
        Schedule flat_schedule{flat_world};
        auto flat_system = std::make_unique<Transform3DSystem>();
        auto* flat_observer = flat_system.get();
        assert(flat_schedule.addSystem(std::move(flat_system)));
        assert(flat_schedule.compile().valid());

        constexpr std::size_t kRootCount = 100'000u;
        auto& flat_registry = flat_world.registry();
        flat_registry.storage<entt::entity>().reserve(kRootCount);
        flat_registry.storage<Transform3DComponent>().reserve(kRootCount);
        for (std::size_t index = 0u; index < kRootCount; ++index)
        {
            const auto entity = flat_registry.create();
            flat_registry.emplace<Transform3DComponent>(
                entity,
                Transform3DComponent{});
        }
        flat_schedule.tick(0.0f);
        assert(flat_observer->lastVisitedCount() == kRootCount);
        flat_schedule.tick(0.0f);
        assert(flat_observer->lastVisitedCount() == 0u);
    }

    return 0;
}
