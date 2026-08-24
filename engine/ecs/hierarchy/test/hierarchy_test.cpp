#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/HierarchySchema.hpp>
#include <lux/engine/ecs/HierarchySystem.hpp>
#include <lux/engine/ecs/Schedule.hpp>

#include <cassert>
#include <memory>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] lux::ecs::SystemHandle<lux::ecs::HierarchySystem>
    installHierarchy(
        lux::ecs::Schedule& schedule,
        lux::ecs::HierarchyIndex& hierarchy
    )
    {
        auto edit_result = schedule.edit();
        assert(edit_result);
        auto edit = std::move(*edit_result);
        auto handle = edit.add(
            std::make_unique<lux::ecs::HierarchySystem>(hierarchy),
            lux::ecs::SystemPhase::PreUpdate
        );
        assert(handle);
        assert(edit.commit());
        return handle;
    }
}

int main()
{
    lux::ecs::World world;
    lux::ecs::HierarchyIndex hierarchy{world};
    {
        lux::ecs::World wrong_world;
        lux::ecs::Schedule wrong_schedule{wrong_world};
        auto wrong_edit_result = wrong_schedule.edit();
        auto wrong_edit = std::move(*wrong_edit_result);
        const auto wrong_handle = wrong_edit.add(
            std::make_unique<lux::ecs::HierarchySystem>(hierarchy),
            lux::ecs::SystemPhase::PreUpdate
        );
        assert(wrong_handle);
        assert(!wrong_edit.commit());
    }
    auto edit_result = world.edit();
    assert(edit_result);
    auto edit = std::move(*edit_result);
    const auto root = edit.create();
    const auto second_root = edit.create();
    const auto child = edit.create();
    const auto grandchild = edit.create();
    assert(lux::ecs::reparent(edit, child, root));
    assert(lux::ecs::reparent(edit, grandchild, child));
    const auto cycle = lux::ecs::reparent(edit, root, grandchild);
    assert(!cycle);
    assert(cycle.error() == lux::ecs::EHierarchyError::CYCLE);
    edit = {};

    assert(!hierarchy.synchronized());
    lux::ecs::Schedule schedule{world};
    const auto hierarchy_handle = installHierarchy(schedule, hierarchy);
    schedule.run(1.0F / 60.0F, 1U);
    assert(hierarchy.synchronized());
    assert(hierarchy.lastError() == lux::ecs::EHierarchyError::NONE);
    assert(hierarchy.parent(child) == root);
    assert(hierarchy.parent(grandchild) == child);
    std::vector<lux::ecs::Entity> root_children;
    for (const lux::ecs::Entity entity : hierarchy.children(root))
        root_children.push_back(entity);
    assert(root_children == std::vector<lux::ecs::Entity>{child});

    lux::ecs::HierarchyChangeCursor hierarchy_cursor;
    assert(
        hierarchy.changes(hierarchy_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );

    auto reparent_result = world.edit();
    auto reparent_edit = std::move(*reparent_result);
    assert(lux::ecs::reparent(reparent_edit, child, second_root));
    assert(!lux::ecs::reparent(reparent_edit, second_root, grandchild));
    reparent_edit = {};
    schedule.run(1.0F / 60.0F, 2U);
    assert(hierarchy.parent(child) == second_root);
    auto reparent_changes = hierarchy.changes(hierarchy_cursor);
    assert(reparent_changes.status() == lux::ecs::EChangeReadStatus::CURRENT);
    assert(reparent_changes.size() == 1U);
    const auto reparent_change = *reparent_changes.begin();
    assert(reparent_change.entity == child);
    assert(reparent_change.previous_parent == root);
    assert(reparent_change.parent == second_root);
    assert(
        reparent_change.kind ==
        lux::ecs::EHierarchyChangeKind::REPARENTED
    );

    auto destroy_parent_result = world.edit();
    auto destroy_parent = std::move(*destroy_parent_result);
    destroy_parent.destroy(child);
    destroy_parent = {};
    schedule.run(1.0F / 60.0F, 3U);
    assert(!world.valid(child));
    assert(world.find<lux::ecs::Parent>(grandchild) == nullptr);
    assert(hierarchy.parent(grandchild) == lux::ecs::NullEntity);

    auto generation_result = world.edit();
    auto generation_edit = std::move(*generation_result);
    const auto replacement = generation_edit.create();
    assert(replacement != child);
    assert(lux::ecs::reparent(generation_edit, replacement, root));
    generation_edit = {};
    schedule.run(1.0F / 60.0F, 4U);
    assert(hierarchy.parent(child) == lux::ecs::NullEntity);
    assert(hierarchy.parent(replacement) == root);
    assert(hierarchy_handle);

    {
        lux::ecs::World subtree_world;
        auto subtree_result = subtree_world.edit();
        auto subtree_edit = std::move(*subtree_result);
        const auto subtree_root = subtree_edit.create();
        const auto first_child = subtree_edit.create();
        const auto second_child = subtree_edit.create();
        const auto leaf = subtree_edit.create();
        assert(lux::ecs::reparent(subtree_edit, first_child, subtree_root));
        assert(lux::ecs::reparent(subtree_edit, second_child, subtree_root));
        assert(lux::ecs::reparent(subtree_edit, leaf, first_child));
        assert(lux::ecs::destroySubtree(subtree_edit, subtree_root));
        assert(!subtree_world.valid(subtree_root));
        assert(!subtree_world.valid(first_child));
        assert(!subtree_world.valid(second_child));
        assert(!subtree_world.valid(leaf));
    }

    {
        lux::ecs::World invalid_world;
        lux::ecs::HierarchyIndex invalid_hierarchy{invalid_world};
        auto invalid_result = invalid_world.edit();
        auto invalid_edit = std::move(*invalid_result);
        const auto first = invalid_edit.create();
        const auto second = invalid_edit.create();
        invalid_edit.emplace<lux::ecs::Parent>(first, second);
        invalid_edit.emplace<lux::ecs::Parent>(second, first);
        invalid_edit = {};
        lux::ecs::Schedule invalid_schedule{invalid_world};
        (void)installHierarchy(invalid_schedule, invalid_hierarchy);
        invalid_schedule.run(1.0F / 60.0F, 1U);
        assert(!invalid_hierarchy.synchronized());
        assert(
            invalid_hierarchy.lastError() ==
            lux::ecs::EHierarchyError::CYCLE
        );
        assert(invalid_hierarchy.size() == 0U);
    }

    {
        lux::ecs::World overflow_world;
        lux::ecs::HierarchyIndex overflow_hierarchy{overflow_world};
        auto root_result = overflow_world.edit();
        auto root_edit = std::move(*root_result);
        const auto overflow_root = root_edit.create();
        root_edit = {};
        lux::ecs::Schedule overflow_schedule{overflow_world};
        (void)installHierarchy(overflow_schedule, overflow_hierarchy);
        overflow_schedule.run(1.0F / 60.0F, 1U);
        lux::ecs::HierarchyChangeCursor overflow_cursor;
        assert(
            overflow_hierarchy.changes(overflow_cursor).status() ==
            lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
        );

        auto children_result = overflow_world.edit();
        auto children_edit = std::move(*children_result);
        for (std::size_t index{}; index < 65537U; ++index)
        {
            const auto entity = children_edit.create();
            assert(lux::ecs::reparent(
                children_edit, entity, overflow_root
            ));
        }
        children_edit = {};
        overflow_schedule.run(1.0F / 60.0F, 2U);
        assert(
            overflow_hierarchy.changes(overflow_cursor).status() ==
            lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
        );
        std::size_t child_count{};
        for (const auto entity : overflow_hierarchy.children(overflow_root))
        {
            (void)entity;
            ++child_count;
        }
        assert(child_count == 65537U);
    }

    assert(lux::ecs::hierarchyComponentSchemas().size() == 1);
}
