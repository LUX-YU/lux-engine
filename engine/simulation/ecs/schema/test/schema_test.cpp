#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>

#include <cassert>
#include <memory>
#include <span>

namespace
{
    struct Position final
    {
        int value{};
    };

    struct DerivedCache final
    {
        int value{};
    };

    struct NonCopy final
    {
        NonCopy() = default;
        NonCopy(const NonCopy&) = delete;
        NonCopy& operator=(const NonCopy&) = delete;
    };
}

int
main()
{
    auto lifetime = std::make_shared<int>(42);
    auto position = lux::simulation::ecs::makeComponentSchema<Position>(
        lux::simulation::ecs::componentSchemaId("test.position"),
        1,
        lux::simulation::ecs::EComponentSnapshotPolicy::COPY,
        lifetime
    );
    auto cache = lux::simulation::ecs::makeComponentSchema<DerivedCache>(
        lux::simulation::ecs::componentSchemaId("test.derived-cache"),
        1,
        lux::simulation::ecs::EComponentSnapshotPolicy::REBUILD
    );

    auto built = lux::simulation::ecs::ComponentSchemaSet::build({position});
    assert(built);
    const auto* stable_pointer = built->find(position.id);
    assert(stable_pointer != nullptr);
    assert(stable_pointer->code_lifetime == lifetime);

    auto extended = built->extended(std::span<const lux::simulation::ecs::ComponentSchema>(&cache, 1U));
    assert(extended);
    assert(extended->all().size() == 2U);
    assert(built->all().size() == 1U);
    assert(built->find(position.id) == stable_pointer);

    auto duplicate = lux::simulation::ecs::ComponentSchemaSet::build({position, position});
    assert(!duplicate);
    assert(duplicate.error().code == lux::simulation::ecs::ESchemaError::DUPLICATE_SCHEMA_ID);

    auto invalid_copy_schema = lux::simulation::ecs::makeComponentSchema<NonCopy>(
        lux::simulation::ecs::componentSchemaId("test.non-copy"),
        1,
        lux::simulation::ecs::EComponentSnapshotPolicy::COPY
    );
    auto invalid_copy = lux::simulation::ecs::ComponentSchemaSet::build({invalid_copy_schema});
    assert(invalid_copy);
}
