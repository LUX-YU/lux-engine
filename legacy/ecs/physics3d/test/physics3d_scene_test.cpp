#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/physics3d/components/Physics3DComponents.hpp>
#include <lux/engine/ecs/physics3d/systems/Physics3DScene.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>

int main()
{
    using namespace lux::ecs;

    Physics3DConfig config;
    config.gravity.setZero();
    config.maximum_bodies = 4096u;
    config.maximum_body_pairs = 8192u;
    config.maximum_contact_constraints = 2048u;
    auto created = Physics3DScene::create(config);
    assert(created);
    auto scene = *created;
    const auto initial_memory = scene->memorySnapshot();
    assert(initial_memory.capacity_bytes >= config.temporary_allocator_bytes);
    assert(initial_memory.allocation_count >= 2u);

    lux::ecs::Registry registry;
    const auto entity = registry.create();
    auto& transform = registry.emplace<Transform3DComponent>(entity);
    transform.position = {
        102'400'007.0,
        9.0,
        76'800'011.0};
    auto& collider = registry.emplace<Collider3DComponent>(entity);
    collider.shape = ECollider3DShape::SPHERE;
    collider.radius = 0.5f;
    auto& rigid = registry.emplace<RigidBody3DComponent>(entity);
    rigid.motion = ERigidBody3DMotion::DYNAMIC;
    rigid.linear_velocity = {6.0f, 0.0f, 0.0f};

    scene->advance(registry, config.fixed_dt);
    assert(scene->dynamicBodyCount() == 1u);
    const auto moved = registry.get<Transform3DComponent>(entity).position;
    assert(moved.x > 102'400'007.05);
    assert(moved.y == 9.0);
    assert(moved.z == 76'800'011.0);

    // Physics identity is the versioned EnTT handle. Neither body needs a
    // persistent/content ID in order to participate in simulation or facts.
    const auto contact_peer = registry.create();
    auto& peer_transform = registry.emplace<Transform3DComponent>(
        contact_peer);
    peer_transform.position = moved;
    peer_transform.position.x += 0.75;
    auto& peer_collider = registry.emplace<Collider3DComponent>(contact_peer);
    peer_collider.shape = ECollider3DShape::SPHERE;
    peer_collider.radius = 0.5f;
    auto& peer_rigid = registry.emplace<RigidBody3DComponent>(contact_peer);
    peer_rigid.motion = ERigidBody3DMotion::STATIC;

    scene->advance(registry, config.fixed_dt);
    assert(scene->dynamicBodyCount() == 2u);
    const auto is_expected_pair = [entity, contact_peer](
        const Physics3DContactFact& fact)
    {
        return (fact.first == entity && fact.second == contact_peer) ||
            (fact.first == contact_peer && fact.second == entity);
    };
    assert(std::ranges::any_of(scene->contacts(), is_expected_pair));
    assert(std::ranges::all_of(
        scene->contacts(),
        [&registry](const Physics3DContactFact& fact)
        {
            return registry.valid(fact.first) && registry.valid(fact.second);
        }));

    // Reusing the entity slot must not turn a queued contact for the old
    // version into a fact about its replacement.
    const auto stale_peer = contact_peer;
    registry.destroy(contact_peer);
    const auto replacement = registry.create();
    assert(replacement != stale_peer);
    assert(entt::to_entity(replacement) == entt::to_entity(stale_peer));
    assert(entt::to_version(replacement) != entt::to_version(stale_peer));
    assert(!registry.valid(stale_peer));
    scene->advance(registry, config.fixed_dt);
    assert(scene->dynamicBodyCount() == 1u);
    assert(std::ranges::none_of(
        scene->contacts(),
        [stale_peer, replacement](const Physics3DContactFact& fact)
        {
            return fact.first == stale_peer || fact.second == stale_peer ||
                fact.first == replacement || fact.second == replacement;
        }));

    constexpr std::uint32_t kSampleEdge = 257u;
    StaticHeightfieldBatch3D heightfield_batch;
    auto& heightfield = heightfield_batch.heightfields.emplace_back();
    heightfield.origin = {100'000'000.0, 0.0, 74'999'808.0};
    heightfield.sample_edge = kSampleEdge;
    heightfield.height_min = -10.0f;
    heightfield.height_max = 20.0f;
    heightfield.sample_spacing = 1.0f;
    heightfield.samples.assign(
        static_cast<std::size_t>(kSampleEdge) * kSampleEdge,
        21845u);
    auto invalid_batch = heightfield_batch;
    invalid_batch.heightfields.emplace_back().sample_edge = 1u;
    auto canceled_batch = heightfield_batch;
    canceled_batch.heightfields.push_back(canceled_batch.heightfields.front());
    canceled_batch.heightfields.push_back(canceled_batch.heightfields.front());
    auto distant_batch = canceled_batch;
    for (auto& field : distant_batch.heightfields)
        field.origin = {1.0e12, 0.0, -1.0e12};
    auto background = preparePhysics3DStaticBatch(
        std::move(heightfield_batch));
    assert(background);
    const auto static_owner = registry.create();
    registry.emplace<StaticColliderBatch3DComponent>(static_owner);
    registry.emplace<ResolvedTransform3DComponent>(static_owner);

    // Canceling after the Nth staged body transfers both that invisible body
    // and every unadopted prepared shape to a bounded retirement lease.
    auto canceled_background = preparePhysics3DStaticBatch(
        std::move(canceled_batch));
    assert(canceled_background);
    auto canceled_stager = scene->beginStaticHeightfieldStaging(
        std::move(*canceled_background), static_owner);
    assert(canceled_stager);
    auto canceled_progress = (*canceled_stager)->advance(1u);
    assert(canceled_progress && !*canceled_progress);
    assert(scene->staticHeightfieldBodyCount() == 1u);
    auto canceled = (*canceled_stager)->cancel();
    (*canceled_stager).reset();
    assert(canceled);
    assert(canceled->remainingBodies() == 1u);
    assert(canceled->remainingRetirementUnits() == 3u);
    assert(!canceled->retireSome(1u));
    assert(scene->staticHeightfieldBodyCount() == 0u);
    assert(canceled->remainingRetirementUnits() == 2u);
    assert(!canceled->retireSome(1u));
    assert(canceled->retireSome(1u));
    canceled.reset();

    // Origin rejection is represented by a failed stager rather than
    // destroying every prepared shape in begin(). The rejected owner remains
    // consumable at exactly the caller's retirement granule.
    auto distant_background = preparePhysics3DStaticBatch(
        std::move(distant_batch));
    assert(distant_background);
    auto distant_stager = scene->beginStaticHeightfieldStaging(
        std::move(*distant_background), static_owner);
    assert(distant_stager);
    auto distant_progress = (*distant_stager)->advance(1u);
    assert(distant_progress && !*distant_progress);
    auto distant_retirement = (*distant_stager)->cancel();
    (*distant_stager).reset();
    assert(distant_retirement);
    assert(distant_retirement->remainingRetirementUnits() == 2u);
    assert(!distant_retirement->retireSome(1u));
    assert(distant_retirement->retireSome(1u));
    distant_retirement.reset();

    auto stager = scene->beginStaticHeightfieldStaging(
        std::move(*background), static_owner);
    assert(stager);
    auto staged = (*stager)->advance(1u);
    assert(staged && *staged);
    auto prepared = (*stager)->finish();
    assert(prepared);
    assert(!prepared->active());
    assert(scene->staticHeightfieldBodyCount() == 1u);
    prepared->activate();
    assert(prepared->active());

    // Static batches map their Jolt bodies back to the complete versioned
    // EnTT owner, so ordinary entities need no persistent identity for
    // dynamic-to-static contact facts.
    const auto static_contact = registry.create();
    auto& static_contact_transform = registry.emplace<Transform3DComponent>(
        static_contact);
    static_contact_transform.position = {
        100'000'001.0, 0.25, 74'999'809.0};
    auto& static_contact_collider = registry.emplace<Collider3DComponent>(
        static_contact);
    static_contact_collider.shape = ECollider3DShape::SPHERE;
    static_contact_collider.radius = 0.5f;
    auto& static_contact_rigid = registry.emplace<RigidBody3DComponent>(
        static_contact);
    static_contact_rigid.motion = ERigidBody3DMotion::DYNAMIC;
    scene->advance(registry, config.fixed_dt);
    const auto is_static_pair = [static_owner, static_contact](
        const Physics3DContactFact& fact)
    {
        return (fact.first == static_owner &&
                fact.second == static_contact) ||
            (fact.first == static_contact && fact.second == static_owner);
    };
    assert(std::ranges::any_of(scene->contacts(), is_static_pair));

    // Reusing the owner slot cannot retarget the private body mapping to the
    // replacement version. Re-adding the body forces a fresh contact edge.
    prepared->deactivate();
    assert(!prepared->active());
    const auto stale_static_owner = static_owner;
    registry.destroy(static_owner);
    const auto static_replacement = registry.create();
    assert(static_replacement != stale_static_owner);
    assert(entt::to_entity(static_replacement) ==
        entt::to_entity(stale_static_owner));
    prepared->activate();
    scene->advance(registry, config.fixed_dt);
    assert(std::ranges::none_of(
        scene->contacts(),
        [stale_static_owner, static_replacement](
            const Physics3DContactFact& fact)
        {
            return fact.first == stale_static_owner ||
                fact.second == stale_static_owner ||
                fact.first == static_replacement ||
                fact.second == static_replacement;
        }));
    prepared->deactivate();
    assert(scene->staticHeightfieldBodyCount() == 1u);
    assert(!prepared->retireSome(0u));
    assert(prepared->remainingBodies() == 1u);
    assert(prepared->retireSome(1u));
    assert(scene->staticHeightfieldBodyCount() == 0u);
    prepared->retire();
    prepared.reset();
    assert(scene->staticHeightfieldBodyCount() == 0u);

    auto rejected = preparePhysics3DStaticBatch(std::move(invalid_batch));
    assert(!rejected);
    assert(scene->staticHeightfieldBodyCount() == 0u);

    const auto character = registry.create();
    auto& character_transform = registry.emplace<Transform3DComponent>(
        character);
    character_transform.position = moved;
    character_transform.position.y += 100.0;
    auto& character_collider = registry.emplace<Collider3DComponent>(character);
    character_collider.shape = ECollider3DShape::CAPSULE;
    character_collider.radius = 0.4f;
    character_collider.half_height = 0.8f;
    auto& controller = registry.emplace<CharacterController3DComponent>(
        character);
    controller.desired_velocity = {3.0f, 0.0f, 0.0f};
    scene->advance(registry, config.fixed_dt);
    assert(scene->characterCount() == 1u);
    assert(registry.get<Transform3DComponent>(character).position.x >
        moved.x);

    registry.destroy(entity);
    registry.destroy(character);
    registry.destroy(replacement);
    registry.destroy(static_contact);
    registry.destroy(static_replacement);
    scene->advance(registry, config.fixed_dt);
    assert(scene->dynamicBodyCount() == 0u);
    assert(scene->characterCount() == 0u);
    assert(scene->droppedContactFactCount() == 0u);
    scene.reset();
    return 0;
}
