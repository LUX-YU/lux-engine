#include <lux/engine/ecs/PersistentEntityIndex.hpp>

#include <array>
#include <cassert>
#include <string_view>
#include <type_traits>
#include <utility>
#include <uuid.h>

namespace
{
    lux::ecs::PersistentEntityId id(std::string_view value)
    {
        return lux::ecs::PersistentEntityId{
            *uuids::uuid::from_string(value)};
    }
}

int main()
{
    static_assert(!std::is_constructible_v<
        lux::ecs::PersistentEntityIdComponent,
        lux::ecs::PersistentEntityId>);
    static_assert(!std::is_copy_assignable_v<
        lux::ecs::PersistentEntityIdComponent>);
    static_assert(!std::is_move_assignable_v<
        lux::ecs::PersistentEntityIdComponent>);
    static_assert(std::is_same_v<
        decltype(std::declval<
            const lux::ecs::PersistentEntityIdComponent&>().id()),
        const lux::ecs::PersistentEntityId&>);

    lux::ecs::Registry registry;
    lux::ecs::PersistentEntityIndex index{registry};
    assert(index.boundTo(registry));

    const auto ordinary = registry.create();
    assert(index.size() == 0u);
    assert(!index.contains(ordinary));

    const auto first = registry.create();
    const auto first_id = id("aaaaaaaa-0000-4000-8000-000000000001");
    assert(lux::ecs::setPersistentEntityId(index, first, first_id));
    assert(index.size() == 1u);
    assert(index.find(first_id) == first);

    const std::array active_ids{first_id};
    const auto active_claim = lux::ecs::claimPersistentEntityIds(
        index, active_ids);
    assert(!active_claim);
    assert(
        active_claim.error() ==
        lux::ecs::EPersistentEntityIdError::DUPLICATE_ID);

    const auto duplicate = registry.create();
    const auto rejected = lux::ecs::setPersistentEntityId(
        index, duplicate, first_id);
    assert(!rejected);
    assert(
        rejected.error() ==
        lux::ecs::EPersistentEntityIdError::DUPLICATE_ID);
    assert(!registry.all_of<lux::ecs::PersistentEntityIdComponent>(duplicate));

    const auto second_id = id("aaaaaaaa-0000-4000-8000-000000000002");
    assert(lux::ecs::setPersistentEntityId(index, first, second_id));
    assert(index.find(first_id) == entt::null);
    assert(index.find(second_id) == first);
    assert(index.size() == 1u);

    // An armed transaction owns a pending claim. Neither another Section nor
    // an ordinary helper write can publish that ID until it commits/cancels.
    const auto claimed_id = id(
        "aaaaaaaa-0000-4000-8000-000000000003");
    const std::array claimed_ids{claimed_id};
    auto pending = lux::ecs::claimPersistentEntityIds(
        index, claimed_ids);
    assert(pending);
    assert(index.pendingCount() == 1u);
    assert(index.claimed(claimed_id));

    auto duplicate_claim = lux::ecs::claimPersistentEntityIds(
        index, claimed_ids);
    assert(!duplicate_claim);
    assert(
        duplicate_claim.error() ==
        lux::ecs::EPersistentEntityIdError::DUPLICATE_ID);

    const auto pending_target = registry.create();
    const auto blocked_set = lux::ecs::setPersistentEntityId(
        index, pending_target, claimed_id);
    assert(!blocked_set);
    assert(
        blocked_set.error() ==
        lux::ecs::EPersistentEntityIdError::DUPLICATE_ID);
    assert(!registry.all_of<lux::ecs::PersistentEntityIdComponent>(
        pending_target));

    pending->reset();
    assert(index.pendingCount() == 0u);
    assert(!index.claimed(claimed_id));

    // Two independently armed Sections can commit in one command barrier.
    // Their reservations must include already pending claims, not only the
    // currently active index size.
    const auto parallel_id_a = id(
        "aaaaaaaa-0000-4000-8000-000000000004");
    const auto parallel_id_b = id(
        "aaaaaaaa-0000-4000-8000-000000000005");
    const std::array parallel_ids_a{parallel_id_a};
    const std::array parallel_ids_b{parallel_id_b};
    auto parallel_claim_a = lux::ecs::claimPersistentEntityIds(
        index, parallel_ids_a);
    auto parallel_claim_b = lux::ecs::claimPersistentEntityIds(
        index, parallel_ids_b);
    assert(parallel_claim_a && parallel_claim_b);
    const auto parallel_entity_a = registry.create();
    const auto parallel_entity_b = registry.create();
    const std::array parallel_entities_a{parallel_entity_a};
    const std::array parallel_entities_b{parallel_entity_b};
    lux::ecs::commitPersistentEntityIds(
        index, *parallel_claim_a, parallel_entities_a);
    lux::ecs::commitPersistentEntityIds(
        index, *parallel_claim_b, parallel_entities_b);
    assert(index.find(parallel_id_a) == parallel_entity_a);
    assert(index.find(parallel_id_b) == parallel_entity_b);
    registry.destroy(parallel_entity_a);
    registry.destroy(parallel_entity_b);

    auto committed = lux::ecs::claimPersistentEntityIds(
        index, claimed_ids);
    assert(committed);
    const std::array committed_entities{pending_target};
    lux::ecs::commitPersistentEntityIds(
        index, *committed, committed_entities);
    assert(!*committed);
    assert(index.pendingCount() == 0u);
    assert(index.find(claimed_id) == pending_target);
    assert(registry.get<lux::ecs::PersistentEntityIdComponent>(
        pending_target).id() == claimed_id);

    // Destroy/remove releases the active identity immediately; it can then be
    // claimed by another generation without retaining a second truth.
    lux::ecs::clearPersistentEntityId(index, pending_target);
    assert(index.find(claimed_id) == entt::null);
    assert(!index.contains(pending_target));
    auto reused = lux::ecs::claimPersistentEntityIds(
        index, claimed_ids);
    assert(reused);
    reused->reset();

    const std::array duplicate_batch_ids{claimed_id, claimed_id};
    auto duplicate_in_batch = lux::ecs::claimPersistentEntityIds(
        index, duplicate_batch_ids);
    assert(!duplicate_in_batch);
    assert(
        duplicate_in_batch.error() ==
        lux::ecs::EPersistentEntityIdError::DUPLICATE_ID);

    const std::array invalid_ids{
        lux::ecs::PersistentEntityId{}};
    auto invalid_claim = lux::ecs::claimPersistentEntityIds(
        index, invalid_ids);
    assert(!invalid_claim);
    assert(
        invalid_claim.error() ==
        lux::ecs::EPersistentEntityIdError::INVALID_ID);

    registry.destroy(first);
    assert(index.find(second_id) == entt::null);
    assert(index.size() == 0u);
    registry.destroy(pending_target);

    // Ordinary entities stay completely outside the sparse identity path.
    assert(registry.valid(ordinary));
    assert(!registry.all_of<lux::ecs::PersistentEntityIdComponent>(ordinary));
    assert(!index.contains(ordinary));
    assert(index.size() == 0u);
    assert(index.pendingCount() == 0u);

    return 0;
}
