#include <lux/engine/ecs/ComponentSchemaId.hpp>
#include <lux/engine/ecs/PersistentEntityId.hpp>

#include <cassert>
#include <string_view>
#include <type_traits>

namespace
{
    struct UnrelatedUuidTag final {};
}

int main()
{
    const auto schema =
        lux::ecs::componentSchemaId("org.lux.test.component");
    assert(lux::ecs::isValidComponentSchemaId(schema));
    assert(!lux::ecs::isCanonicalComponentSchemaName(
        "Org.lux.test.component"));

    auto invalid_hash = schema;
    ++invalid_hash.hash;
    assert(!lux::ecs::isValidComponentSchemaId(invalid_hash));

    const auto uuid = uuids::uuid::from_string(
        "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    assert(uuid);

    const lux::ecs::PersistentEntityId persistent{*uuid};
    assert(!persistent.empty());
    assert(lux::ecs::PersistentEntityRef{persistent}.valid());

    using UnrelatedUuid = lux::ecs::UuidId<UnrelatedUuidTag>;
    static_assert(!std::is_same_v<
        lux::ecs::PersistentEntityId,
        UnrelatedUuid>);
    static_assert(!std::is_convertible_v<
        lux::ecs::PersistentEntityId,
        UnrelatedUuid>);
    return 0;
}
