#include <lux/engine/ecs/SystemConcept.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>

#include <cassert>
#include <type_traits>

namespace
{
    struct Position final
    {
        float value{};
    };

    struct Velocity final
    {
        float value{};
    };

    class MovementSystem final
        : public lux::ecs::StaticSystemAccess<
            lux::ecs::Read<Velocity>,
            lux::ecs::Write<Position>
        >
    {
    public:
        explicit MovementSystem(int value) noexcept : value_(value) {}

        void update(lux::ecs::SystemContext&) noexcept {}

    private:
        int value_{};
    };

    class MissingAccess final
    {
    public:
        void update(lux::ecs::SystemContext&) noexcept {}
    };

    class ThrowingUpdate final
        : public lux::ecs::StaticSystemAccess<lux::ecs::Read<Position>>
    {
    public:
        void update(lux::ecs::SystemContext&) {}
    };

    class AffineSystem final
        : public lux::ecs::StaticSystemAccess<lux::ecs::Read<Position>>
    {
    public:
        using lux_thread_affine = std::true_type;

        explicit AffineSystem(bool valid) noexcept : valid_(valid) {}

        [[nodiscard]] bool isOnAffinityThread() const noexcept
        {
            return valid_;
        }

        void update(lux::ecs::SystemContext&) noexcept {}

    private:
        bool valid_{};
    };

    class LifetimeSystem final
        : public lux::ecs::StaticSystemAccess<>
    {
    public:
        explicit LifetimeSystem(int& destructions) noexcept
            : destructions_(std::addressof(destructions))
        {
        }

        ~LifetimeSystem()
        {
            ++*destructions_;
        }

        void update(lux::ecs::SystemContext&) noexcept {}

    private:
        int* destructions_{};
    };
}

static_assert(lux::ecs::System<MovementSystem>);
static_assert(!lux::ecs::System<MissingAccess>);
static_assert(!lux::ecs::System<ThrowingUpdate>);

int main()
{
    lux::ecs::SystemRegistry systems;
    const auto first = systems.emplace<MovementSystem>(1);
    const auto second = systems.emplace<MovementSystem>(2);
    assert(first && second);
    assert(*first != *second);
    assert(systems.size() == 2U);
    assert(systems.contains(*first));
    assert(systems.contains(*second));

    lux::ecs::SystemRelations relations;
    assert(relations.before(*first, *second));
    assert(!relations.before(*first, *second));
    assert(relations.size() == 1U);

    const auto stale = *first;
    assert(systems.erase(*first));
    assert(!systems.contains(stale));

    const auto replacement = systems.emplace<MovementSystem>(3);
    assert(replacement);
    assert(replacement->slot.index == stale.slot.index);
    assert(replacement->slot.gen != stale.slot.gen);
    assert(relations.before(stale, *replacement));
    const auto invalid_relation = lux::ecs::compileSystemTaskGraph(
        systems,
        relations
    );
    assert(!invalid_relation);
    assert(
        invalid_relation.error().code ==
        lux::ecs::ESystemError::INVALID_SYSTEM
    );

    lux::ecs::SystemRegistry affinity_systems;
    const auto affine = affinity_systems.emplace<AffineSystem>(true);
    assert(affine);
    const auto rejected_affine = affinity_systems.emplace<AffineSystem>(false);
    assert(rejected_affine);
    lux::ecs::SystemRelations affinity_relations;
    const auto affinity_failure = lux::ecs::compileSystemTaskGraph(
        affinity_systems,
        affinity_relations
    );
    assert(!affinity_failure);
    assert(
        affinity_failure.error().code ==
        lux::ecs::ESystemError::EXECUTION_AFFINITY_MISMATCH
    );

    lux::ecs::SystemRegistry foreign_left;
    lux::ecs::SystemRegistry foreign_right;
    const auto foreign_left_id = foreign_left.emplace<MovementSystem>(4);
    const auto foreign_right_id = foreign_right.emplace<MovementSystem>(4);
    assert(foreign_left_id && foreign_right_id);
    assert(foreign_left_id->slot == foreign_right_id->slot);
    assert(foreign_left_id->owner != foreign_right_id->owner);
    assert(!foreign_right.contains(*foreign_left_id));
    assert(!foreign_right.erase(*foreign_left_id));

    lux::ecs::SystemRelations foreign_relations;
    assert(foreign_relations.before(*foreign_left_id, *foreign_right_id));
    const auto foreign_failure = lux::ecs::compileSystemTaskGraph(
        foreign_left,
        foreign_relations
    );
    assert(!foreign_failure);
    assert(
        foreign_failure.error().code ==
        lux::ecs::ESystemError::INVALID_SYSTEM
    );

    const auto relations_id = foreign_relations.id();
    lux::ecs::SystemRelations moved_relations(std::move(foreign_relations));
    assert(moved_relations.id() == relations_id);
    assert(!foreign_relations.id().isValid());
    assert(foreign_relations.before(*foreign_left_id, *foreign_right_id));
    assert(foreign_relations.id() != relations_id);

    const auto moved_id = *replacement;
    lux::ecs::SystemRegistry moved_to(std::move(systems));
    assert(moved_to.contains(moved_id));
    assert(!systems.contains(moved_id));
    const auto reused_source = systems.emplace<MovementSystem>(5);
    assert(reused_source);
    assert(reused_source->owner != moved_id.owner);

    int destructions{};
    lux::ecs::SystemRegistry lifetime_systems;
    lux::ecs::SystemRelations lifetime_relations;
    const auto lifetime_id = lifetime_systems.emplace<LifetimeSystem>(
        destructions
    );
    assert(lifetime_id);
    auto old_compilation = lux::ecs::compileSystemTaskGraph(
        lifetime_systems,
        lifetime_relations
    );
    assert(old_compilation);
    assert(lifetime_systems.erase(*lifetime_id));
    assert(destructions == 1);
    assert(old_compilation->systemCount() == 1U);
}
