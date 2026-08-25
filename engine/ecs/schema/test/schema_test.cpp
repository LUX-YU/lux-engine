#include <lux/engine/ecs/ComponentSchemaSet.hpp>

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

int main()
{
    auto lifetime = std::make_shared<int>(42);
    auto position = lux::ecs::makeComponentSchema<Position>(
        lux::ecs::componentSchemaId("test.position"),
        1,
        lux::ecs::EComponentSnapshotPolicy::COPY,
        lifetime
    );
    auto cache = lux::ecs::makeComponentSchema<DerivedCache>(
        lux::ecs::componentSchemaId("test.derived-cache"),
        1,
        lux::ecs::EComponentSnapshotPolicy::REBUILD
    );

    auto built = lux::ecs::ComponentSchemaSet::build({position});
    assert(built);
    const auto* stable_pointer = built->find(position.id);
    assert(stable_pointer != nullptr);
    assert(stable_pointer->code_lifetime == lifetime);

    auto extended = built->extended(
        std::span<const lux::ecs::ComponentSchema>(&cache, 1U)
    );
    assert(extended);
    assert(extended->all().size() == 2U);
    assert(built->all().size() == 1U);
    assert(built->find(position.id) == stable_pointer);

    auto duplicate = lux::ecs::ComponentSchemaSet::build({position, position});
    assert(!duplicate);
    assert(duplicate.error().code ==
        lux::ecs::ESchemaError::DUPLICATE_SCHEMA_ID);

    auto invalid_copy_schema = lux::ecs::makeComponentSchema<NonCopy>(
        lux::ecs::componentSchemaId("test.non-copy"),
        1,
        lux::ecs::EComponentSnapshotPolicy::COPY
    );
    auto invalid_copy = lux::ecs::ComponentSchemaSet::build(
        {invalid_copy_schema}
    );
    assert(invalid_copy);
}
