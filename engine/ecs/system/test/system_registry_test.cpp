#include <lux/engine/ecs/SystemConcept.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>

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

        void requestStop() noexcept
        {
            stopped_ = true;
        }

        [[nodiscard]] bool stopped() const noexcept
        {
            return stopped_;
        }

    private:
        int value_{};
        bool stopped_{};
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

    lux::ecs::SystemRelations relations(systems);
    assert(relations.before(*first, *second));
    assert(!relations.before(*first, *second));
    assert(relations.size() == 1U);

    assert(!systems.stopped(*first));
    assert(systems.requestStop(*first));
    assert(systems.stopped(*first));

    const auto stale = *first;
    assert(systems.erase(*first));
    assert(!systems.contains(stale));

    const auto replacement = systems.emplace<MovementSystem>(3);
    assert(replacement);
    assert(replacement->index == stale.index);
    assert(replacement->gen != stale.gen);
    assert(!relations.before(stale, *replacement));

    const auto affine = systems.emplace<AffineSystem>(true);
    assert(affine);
    const auto rejected_affine = systems.emplace<AffineSystem>(false);
    assert(!rejected_affine);
    assert(
        rejected_affine.error().code ==
        lux::ecs::ESystemError::EXECUTION_AFFINITY_MISMATCH
    );
}
