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
    constexpr lux::simulation::ecs::EcsChangeHistoryBudget kHistoryBudget{
        256U * 1024U,
        32U * 1024U * 1024U
    };
    constexpr lux::simulation::ecs::testing::EcsTaskTestRigCapacity
        kRigCapacity{
            131072U,
            lux::simulation::ecs::EcsCommandProducerCapacity{
                131072U,
                8U * 1024U * 1024U
            }
        };
    constexpr std::size_t kHierarchyBatchCapacity = 131072U;

[[nodiscard]] lux::simulation::SystemId installHierarchy(
        lux::simulation::ecs::EcsState& world,
        lux::simulation::ecs::testing::EcsTaskTestRig& schedule,
        lux::simulation::ecs::HierarchyIndex& hierarchy,
        lux::simulation::ecs::HierarchyMutationBatch& mutations,
        lux::simulation::ecs::HierarchyDeltaBatch& deltas
    )
    {
        const auto handle = schedule.add<lux::simulation::ecs::HierarchySystem>(
            world,
            hierarchy,
            mutations,
            deltas
        );
        assert(schedule.compile());
        return handle;
    }
}

int main()
{
    lux::simulation::ecs::EcsState world;
    lux::simulation::ecs::HierarchyIndex hierarchy;
    lux::simulation::ecs::HierarchyMutationBatch hierarchy_mutations;
    lux::simulation::ecs::HierarchyDeltaBatch hierarchy_deltas;
    assert(hierarchy_mutations.prepare(kHierarchyBatchCapacity));
    assert(hierarchy_deltas.prepare(kHierarchyBatchCapacity));
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
    lux::simulation::ecs::testing::EcsTaskTestRig schedule{
        world,
        kHistoryBudget,
        kRigCapacity
    };
    const auto hierarchy_handle = installHierarchy(
        world,
        schedule,
        hierarchy,
        hierarchy_mutations,
        hierarchy_deltas
    );
    assert(schedule.run(1.0F / 60.0F, 1U));
    assert(hierarchy.synchronized());
    assert(hierarchy.lastError() == lux::simulation::ecs::EHierarchyError::NONE);
    assert(hierarchy.parent(child) == root);
    assert(hierarchy.parent(grandchild) == child);
    std::vector<lux::simulation::ecs::Entity> root_children;
    for (const lux::simulation::ecs::Entity entity : hierarchy.children(root))
        root_children.push_back(entity);
    assert(root_children == std::vector<lux::simulation::ecs::Entity>{child});

    assert(!hierarchy_deltas.exact());

    auto reparent_result = schedule.mutate();
    auto reparent_edit = std::move(*reparent_result);
    assert(lux::simulation::ecs::reparent(reparent_edit, child, second_root));
    assert(!lux::simulation::ecs::reparent(reparent_edit, second_root, grandchild));
    reparent_edit = {};
    assert(schedule.run(1.0F / 60.0F, 2U));
    assert(hierarchy.parent(child) == second_root);
    assert(hierarchy_deltas.exact());
    assert(hierarchy_deltas.values().size() == 1U);
    const auto reparent_change = hierarchy_deltas.values().front();
    assert(reparent_change.entity == child);
    assert(reparent_change.previous_parent == root);
    assert(reparent_change.parent == second_root);
    assert(
        reparent_change.kind ==
        lux::simulation::ecs::EHierarchyDeltaKind::REPARENTED
    );

    auto destroy_parent_result = schedule.mutate();
    auto destroy_parent = std::move(*destroy_parent_result);
    destroy_parent.destroy(child);
    destroy_parent = {};
    schedule.failNextCommandPush(hierarchy_handle);
    assert(!schedule.run(1.0F / 60.0F, 3U));
    assert(!world.valid(child));
    assert(world.find<lux::simulation::ecs::Parent>(grandchild) != nullptr);
    assert(!hierarchy.synchronized());
    assert(
        hierarchy.lastError() ==
        lux::simulation::ecs::EHierarchyError::CAPACITY_EXCEEDED
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

        lux::simulation::ecs::HierarchyIndex stale_hierarchy;
        lux::simulation::ecs::HierarchyMutationBatch stale_mutations;
        lux::simulation::ecs::HierarchyDeltaBatch stale_deltas;
        assert(stale_mutations.prepare(kHierarchyBatchCapacity));
        assert(stale_deltas.prepare(kHierarchyBatchCapacity));
        lux::simulation::ecs::testing::EcsTaskTestRig stale_schedule{
            stale_world,
            kHistoryBudget,
            kRigCapacity
        };
        (void)installHierarchy(
            stale_world,
            stale_schedule,
            stale_hierarchy,
            stale_mutations,
            stale_deltas
        );
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
        lux::simulation::ecs::HierarchyIndex repair_hierarchy;
        lux::simulation::ecs::HierarchyMutationBatch repair_mutations;
        lux::simulation::ecs::HierarchyDeltaBatch repair_deltas;
        assert(repair_mutations.prepare(kHierarchyBatchCapacity));
        assert(repair_deltas.prepare(kHierarchyBatchCapacity));
        lux::simulation::ecs::testing::EcsTaskTestRig repair_schedule{
            repair_world,
            kHistoryBudget,
            kRigCapacity
        };
        const auto repair_handle = installHierarchy(
            repair_world,
            repair_schedule,
            repair_hierarchy,
            repair_mutations,
            repair_deltas
        );
        assert(repair_schedule.run(1.0F / 60.0F, 1U));
        auto destroy_result = repair_schedule.mutate();
        auto destroy = std::move(*destroy_result);
        destroy.destroy(doomed_parent);
        destroy = {};
        repair_schedule.failNextCommandPush(repair_handle);
        assert(!repair_schedule.run(1.0F / 60.0F, 2U));
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
        lux::simulation::ecs::HierarchyIndex cancel_hierarchy;
        lux::simulation::ecs::HierarchyMutationBatch cancel_mutations;
        lux::simulation::ecs::HierarchyDeltaBatch cancel_deltas;
        assert(cancel_mutations.prepare(kHierarchyBatchCapacity));
        assert(cancel_deltas.prepare(kHierarchyBatchCapacity));
        lux::simulation::ecs::testing::EcsTaskTestRig cancel_schedule{
            cancel_world,
            kHistoryBudget,
            kRigCapacity
        };
        const auto cancel_handle = installHierarchy(
            cancel_world,
            cancel_schedule,
            cancel_hierarchy,
            cancel_mutations,
            cancel_deltas
        );
        assert(cancel_schedule.run(1.0F / 60.0F, 1U));
        auto destroy_result = cancel_schedule.mutate();
        auto destroy = std::move(*destroy_result);
        destroy.destroy(doomed_parent);
        destroy = {};
        cancel_schedule.failNextCommandPush(cancel_handle);
        assert(!cancel_schedule.run(1.0F / 60.0F, 2U));
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
        lux::simulation::ecs::HierarchyIndex fabricated_hierarchy;
        lux::simulation::ecs::HierarchyMutationBatch fabricated_mutations;
        lux::simulation::ecs::HierarchyDeltaBatch fabricated_deltas;
        assert(fabricated_mutations.prepare(kHierarchyBatchCapacity));
        assert(fabricated_deltas.prepare(kHierarchyBatchCapacity));
        lux::simulation::ecs::testing::EcsTaskTestRig fabricated_schedule{
            fabricated_world,
            kHistoryBudget,
            kRigCapacity
        };
        (void)installHierarchy(
            fabricated_world,
            fabricated_schedule,
            fabricated_hierarchy,
            fabricated_mutations,
            fabricated_deltas
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
        lux::simulation::ecs::HierarchyIndex deep_hierarchy;
        lux::simulation::ecs::HierarchyMutationBatch deep_mutations;
        lux::simulation::ecs::HierarchyDeltaBatch deep_deltas;
        assert(deep_mutations.prepare(kHierarchyBatchCapacity));
        assert(deep_deltas.prepare(kHierarchyBatchCapacity));
        lux::simulation::ecs::testing::EcsTaskTestRig deep_schedule{
            deep_world,
            kHistoryBudget,
            kRigCapacity
        };
        (void)installHierarchy(
            deep_world,
            deep_schedule,
            deep_hierarchy,
            deep_mutations,
            deep_deltas
        );
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
        lux::simulation::ecs::HierarchyIndex invalid_hierarchy;
        lux::simulation::ecs::HierarchyMutationBatch invalid_mutations;
        lux::simulation::ecs::HierarchyDeltaBatch invalid_deltas;
        assert(invalid_mutations.prepare(kHierarchyBatchCapacity));
        assert(invalid_deltas.prepare(kHierarchyBatchCapacity));
        auto invalid_result = invalid_world.mutate();
        auto invalid_edit = std::move(*invalid_result);
        const auto first = invalid_edit.create();
        const auto second = invalid_edit.create();
        invalid_edit.emplace<lux::simulation::ecs::Parent>(first, second);
        invalid_edit.emplace<lux::simulation::ecs::Parent>(second, first);
        invalid_edit = {};
        lux::simulation::ecs::testing::EcsTaskTestRig invalid_schedule{
            invalid_world,
            kHistoryBudget,
            kRigCapacity
        };
        (void)installHierarchy(
            invalid_world,
            invalid_schedule,
            invalid_hierarchy,
            invalid_mutations,
            invalid_deltas
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
        lux::simulation::ecs::HierarchyIndex overflow_hierarchy;
        lux::simulation::ecs::HierarchyMutationBatch overflow_mutations;
        lux::simulation::ecs::HierarchyDeltaBatch overflow_deltas;
        assert(overflow_mutations.prepare(kHierarchyBatchCapacity));
        assert(overflow_deltas.prepare(65536U));
        auto root_result = overflow_world.mutate();
        auto root_edit = std::move(*root_result);
        const auto overflow_root = root_edit.create();
        root_edit = {};
        lux::simulation::ecs::testing::EcsTaskTestRig overflow_schedule{
            overflow_world,
            kHistoryBudget,
            kRigCapacity
        };
        (void)installHierarchy(
            overflow_world,
            overflow_schedule,
            overflow_hierarchy,
            overflow_mutations,
            overflow_deltas
        );
        assert(overflow_schedule.run(1.0F / 60.0F, 1U));
        assert(!overflow_deltas.exact());

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
        assert(!overflow_deltas.exact());
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
