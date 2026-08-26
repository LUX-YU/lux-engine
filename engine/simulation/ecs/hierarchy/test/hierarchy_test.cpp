#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/HierarchySystem.hpp>
#include <lux/engine/simulation/ecs/hierarchy/detail/HierarchyIndexTestAccess.hpp>
#include <lux/engine/simulation/ecs/task/support/EcsTaskTestRig.hpp>

#include <cassert>
#include <memory>
#include <utility>
#include <vector>

namespace
{
[[nodiscard]] lux::simulation::SystemId installHierarchy(
        lux::simulation::ecs::EcsState& world,
        lux::simulation::ecs::testing::EcsTaskTestRig& schedule,
        lux::simulation::ecs::HierarchyIndex& hierarchy
    )
    {
        const auto handle = schedule.add<lux::simulation::ecs::HierarchySystem>(
            world,
            hierarchy
        );
        assert(schedule.compile());
        return handle;
    }
}

int main()
{
    lux::simulation::ecs::EcsState world;
    lux::simulation::ecs::HierarchyIndex hierarchy{world};
    auto edit_result = world.mutate();
    assert(edit_result);
    auto edit = std::move(*edit_result);
    const auto root = edit.create();
    const auto second_root = edit.create();
    const auto child = edit.create();
    const auto grandchild = edit.create();
    assert(lux::simulation::ecs::reparent(edit, child, root));
    assert(lux::simulation::ecs::reparent(edit, grandchild, child));
    const auto cycle = lux::simulation::ecs::reparent(edit, root, grandchild);
    assert(!cycle);
    assert(cycle.error() == lux::simulation::ecs::EHierarchyError::CYCLE);
    edit = {};

    assert(!hierarchy.synchronized());
    lux::simulation::ecs::testing::EcsTaskTestRig schedule{world};
    const auto hierarchy_handle = installHierarchy(world, schedule, hierarchy);
    assert(schedule.run(1.0F / 60.0F, 1U));
    assert(hierarchy.synchronized());
    assert(hierarchy.lastError() == lux::simulation::ecs::EHierarchyError::NONE);
    assert(hierarchy.parent(child) == root);
    assert(hierarchy.parent(grandchild) == child);
    std::vector<lux::simulation::ecs::Entity> root_children;
    for (const lux::simulation::ecs::Entity entity : hierarchy.children(root))
        root_children.push_back(entity);
    assert(root_children == std::vector<lux::simulation::ecs::Entity>{child});

    lux::simulation::ecs::HierarchyChangeCursor hierarchy_cursor;
    assert(
        hierarchy.changes(hierarchy_cursor).status() ==
        lux::simulation::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );

    auto reparent_result = schedule.mutate();
    auto reparent_edit = std::move(*reparent_result);
    assert(lux::simulation::ecs::reparent(reparent_edit, child, second_root));
    assert(!lux::simulation::ecs::reparent(reparent_edit, second_root, grandchild));
    reparent_edit = {};
    assert(schedule.run(1.0F / 60.0F, 2U));
    assert(hierarchy.parent(child) == second_root);
    auto reparent_changes = hierarchy.changes(hierarchy_cursor);
    assert(reparent_changes.status() == lux::simulation::ecs::EChangeReadStatus::CURRENT);
    assert(reparent_changes.size() == 1U);
    const auto reparent_change = *reparent_changes.begin();
    assert(reparent_change.entity == child);
    assert(reparent_change.previous_parent == root);
    assert(reparent_change.parent == second_root);
    assert(
        reparent_change.kind ==
        lux::simulation::ecs::EHierarchyChangeKind::REPARENTED
    );

    auto destroy_parent_result = schedule.mutate();
    auto destroy_parent = std::move(*destroy_parent_result);
    destroy_parent.destroy(child);
    destroy_parent = {};
    schedule.failNextCommandPush(hierarchy_handle);
    assert(schedule.run(1.0F / 60.0F, 3U));
    assert(!world.valid(child));
    assert(world.find<lux::simulation::ecs::Parent>(grandchild) != nullptr);
    assert(!hierarchy.synchronized());
    assert(
        hierarchy.lastError() ==
        lux::simulation::ecs::EHierarchyError::ALLOCATION_FAILURE
    );
    assert(schedule.run(1.0F / 60.0F, 4U));
    assert(world.find<lux::simulation::ecs::Parent>(grandchild) == nullptr);
    assert(!hierarchy.synchronized());
    assert(schedule.run(1.0F / 60.0F, 5U));
    assert(hierarchy.synchronized());
    assert(hierarchy.parent(grandchild) == lux::simulation::ecs::NullEntity);

    auto generation_result = schedule.mutate();
    auto generation_edit = std::move(*generation_result);
    const auto replacement = generation_edit.create();
    assert(replacement != child);
    assert(lux::simulation::ecs::reparent(generation_edit, replacement, root));
    generation_edit = {};
    assert(schedule.run(1.0F / 60.0F, 6U));
    assert(hierarchy.parent(child) == lux::simulation::ecs::NullEntity);
    assert(hierarchy.parent(replacement) == root);

    {
        // A Parent can become stale while no HierarchySystem is installed.
        // The first cold rebuild must classify the old generation as a
        // retryable repair instead of permanently rejecting canonical data.
        lux::simulation::ecs::EcsState stale_world;
        auto stale_result = stale_world.mutate();
        auto stale_edit = std::move(*stale_result);
        const auto stale_parent = stale_edit.create();
        const auto stale_child = stale_edit.create();
        stale_edit.emplace<lux::simulation::ecs::Parent>(stale_child, stale_parent);
        stale_edit.destroy(stale_parent);
        const auto reused_parent_slot = stale_edit.create();
        assert(reused_parent_slot != stale_parent);
        stale_edit = {};

        lux::simulation::ecs::HierarchyIndex stale_hierarchy{stale_world};
        lux::simulation::ecs::testing::EcsTaskTestRig stale_schedule{stale_world};
        (void)installHierarchy(stale_world, stale_schedule, stale_hierarchy);
        assert(stale_schedule.run(1.0F / 60.0F, 1U));
        assert(!stale_hierarchy.synchronized());
        assert(stale_schedule.run(1.0F / 60.0F, 2U));
        assert(stale_hierarchy.synchronized());
        assert(stale_world.find<lux::simulation::ecs::Parent>(stale_child) == nullptr);
    }

    {
        // An authored replacement wins over a pending orphan repair.
        lux::simulation::ecs::EcsState repair_world;
        auto setup_result = repair_world.mutate();
        auto setup = std::move(*setup_result);
        const auto doomed_parent = setup.create();
        const auto replacement_parent = setup.create();
        const auto repair_child = setup.create();
        setup.emplace<lux::simulation::ecs::Parent>(repair_child, doomed_parent);
        setup = {};
        lux::simulation::ecs::HierarchyIndex repair_hierarchy{repair_world};
        lux::simulation::ecs::testing::EcsTaskTestRig repair_schedule{repair_world};
        const auto repair_handle = installHierarchy(
            repair_world, repair_schedule, repair_hierarchy
        );
        assert(repair_schedule.run(1.0F / 60.0F, 1U));
        auto destroy_result = repair_schedule.mutate();
        auto destroy = std::move(*destroy_result);
        destroy.destroy(doomed_parent);
        destroy = {};
        repair_schedule.failNextCommandPush(repair_handle);
        assert(repair_schedule.run(1.0F / 60.0F, 2U));
        auto replace_result = repair_schedule.mutate();
        auto replace = std::move(*replace_result);
        replace.update<lux::simulation::ecs::Parent>(
            repair_child,
            [replacement_parent](lux::simulation::ecs::Parent& parent) noexcept
            {
                parent.entity = replacement_parent;
            }
        );
        replace = {};
        assert(repair_schedule.run(1.0F / 60.0F, 3U));
        assert(repair_hierarchy.synchronized());
        assert(repair_hierarchy.parent(repair_child) == replacement_parent);
    }

    {
        // Destroying the child cancels its embedded repair without leaving a
        // dangling intrusive queue entry.
        lux::simulation::ecs::EcsState cancel_world;
        auto setup_result = cancel_world.mutate();
        auto setup = std::move(*setup_result);
        const auto doomed_parent = setup.create();
        const auto repair_child = setup.create();
        setup.emplace<lux::simulation::ecs::Parent>(repair_child, doomed_parent);
        setup = {};
        lux::simulation::ecs::HierarchyIndex cancel_hierarchy{cancel_world};
        lux::simulation::ecs::testing::EcsTaskTestRig cancel_schedule{cancel_world};
        const auto cancel_handle = installHierarchy(
            cancel_world, cancel_schedule, cancel_hierarchy
        );
        assert(cancel_schedule.run(1.0F / 60.0F, 1U));
        auto destroy_result = cancel_schedule.mutate();
        auto destroy = std::move(*destroy_result);
        destroy.destroy(doomed_parent);
        destroy = {};
        cancel_schedule.failNextCommandPush(cancel_handle);
        assert(cancel_schedule.run(1.0F / 60.0F, 2U));
        auto cancel_result = cancel_schedule.mutate();
        auto cancel = std::move(*cancel_result);
        cancel.destroy(repair_child);
        cancel = {};
        assert(cancel_schedule.run(1.0F / 60.0F, 3U));
        assert(cancel_hierarchy.synchronized());
    }

    {
        lux::simulation::ecs::EcsState fabricated_world;
        auto fabricated_result = fabricated_world.mutate();
        auto fabricated_edit = std::move(*fabricated_result);
        const auto child_entity = fabricated_edit.create();
        fabricated_edit.emplace<lux::simulation::ecs::Parent>(
            child_entity,
            static_cast<lux::simulation::ecs::Entity>(999999U)
        );
        fabricated_edit = {};
        lux::simulation::ecs::HierarchyIndex fabricated_hierarchy{fabricated_world};
        lux::simulation::ecs::testing::EcsTaskTestRig fabricated_schedule{fabricated_world};
        (void)installHierarchy(
            fabricated_world,
            fabricated_schedule,
            fabricated_hierarchy
        );
        assert(fabricated_schedule.run(1.0F / 60.0F, 1U));
        assert(!fabricated_hierarchy.synchronized());
        assert(
            fabricated_hierarchy.lastError() ==
            lux::simulation::ecs::EHierarchyError::INVALID_PARENT
        );
    }

    {
        // Cold deep-chain construction validates each relation a fixed number
        // of times; it must not invoke the incremental ancestor validator.
        constexpr std::size_t kDepth = 10000U;
        lux::simulation::ecs::EcsState deep_world;
        auto deep_result = deep_world.mutate();
        auto deep_edit = std::move(*deep_result);
        auto previous = deep_edit.create();
        for (std::size_t index{1U}; index < kDepth; ++index)
        {
            const auto entity = deep_edit.create();
            deep_edit.emplace<lux::simulation::ecs::Parent>(entity, previous);
            previous = entity;
        }
        deep_edit = {};
        lux::simulation::ecs::HierarchyIndex deep_hierarchy{deep_world};
        lux::simulation::ecs::testing::EcsTaskTestRig deep_schedule{deep_world};
        (void)installHierarchy(deep_world, deep_schedule, deep_hierarchy);
        assert(deep_schedule.run(1.0F / 60.0F, 1U));
        assert(deep_hierarchy.synchronized());
        assert(
            lux::simulation::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                deep_hierarchy
            ) == kDepth - 1U
        );
        assert(deep_schedule.run(1.0F / 60.0F, 2U));
        assert(
            lux::simulation::ecs::detail::HierarchyIndexTestAccess::visitedNodes(
                deep_hierarchy
            ) == 0U
        );
    }

    {
        lux::simulation::ecs::EcsState subtree_world;
        auto subtree_result = subtree_world.mutate();
        auto subtree_edit = std::move(*subtree_result);
        const auto subtree_root = subtree_edit.create();
        const auto first_child = subtree_edit.create();
        const auto second_child = subtree_edit.create();
        const auto leaf = subtree_edit.create();
        assert(lux::simulation::ecs::reparent(subtree_edit, first_child, subtree_root));
        assert(lux::simulation::ecs::reparent(subtree_edit, second_child, subtree_root));
        assert(lux::simulation::ecs::reparent(subtree_edit, leaf, first_child));
        assert(lux::simulation::ecs::destroySubtree(subtree_edit, subtree_root));
        assert(!subtree_world.valid(subtree_root));
        assert(!subtree_world.valid(first_child));
        assert(!subtree_world.valid(second_child));
        assert(!subtree_world.valid(leaf));
    }

    {
        lux::simulation::ecs::EcsState invalid_world;
        lux::simulation::ecs::HierarchyIndex invalid_hierarchy{invalid_world};
        auto invalid_result = invalid_world.mutate();
        auto invalid_edit = std::move(*invalid_result);
        const auto first = invalid_edit.create();
        const auto second = invalid_edit.create();
        invalid_edit.emplace<lux::simulation::ecs::Parent>(first, second);
        invalid_edit.emplace<lux::simulation::ecs::Parent>(second, first);
        invalid_edit = {};
        lux::simulation::ecs::testing::EcsTaskTestRig invalid_schedule{invalid_world};
        (void)installHierarchy(
            invalid_world,
            invalid_schedule,
            invalid_hierarchy
        );
        assert(invalid_schedule.run(1.0F / 60.0F, 1U));
        assert(!invalid_hierarchy.synchronized());
        assert(
            invalid_hierarchy.lastError() ==
            lux::simulation::ecs::EHierarchyError::CYCLE
        );
        assert(invalid_hierarchy.size() == 0U);
    }

    {
        lux::simulation::ecs::EcsState overflow_world;
        lux::simulation::ecs::HierarchyIndex overflow_hierarchy{overflow_world};
        auto root_result = overflow_world.mutate();
        auto root_edit = std::move(*root_result);
        const auto overflow_root = root_edit.create();
        root_edit = {};
        lux::simulation::ecs::testing::EcsTaskTestRig overflow_schedule{overflow_world};
        (void)installHierarchy(
            overflow_world,
            overflow_schedule,
            overflow_hierarchy
        );
        assert(overflow_schedule.run(1.0F / 60.0F, 1U));
        lux::simulation::ecs::HierarchyChangeCursor overflow_cursor;
        assert(
            overflow_hierarchy.changes(overflow_cursor).status() ==
            lux::simulation::ecs::EChangeReadStatus::RESYNC_REQUIRED
        );

        auto children_result = overflow_schedule.mutate();
        auto children_edit = std::move(*children_result);
        for (std::size_t index{}; index < 65537U; ++index)
        {
            const auto entity = children_edit.create();
            assert(lux::simulation::ecs::reparent(
                children_edit, entity, overflow_root
            ));
        }
        children_edit = {};
        assert(overflow_schedule.run(1.0F / 60.0F, 2U));
        assert(
            overflow_hierarchy.changes(overflow_cursor).status() ==
            lux::simulation::ecs::EChangeReadStatus::RESYNC_REQUIRED
        );
        std::size_t child_count{};
        for (const auto entity : overflow_hierarchy.children(overflow_root))
        {
            (void)entity;
            ++child_count;
        }
        assert(child_count == 65537U);
    }

    assert(lux::simulation::ecs::hierarchyComponentSchemas().size() == 1);
}
