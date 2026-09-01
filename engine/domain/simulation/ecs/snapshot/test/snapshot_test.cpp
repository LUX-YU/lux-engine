#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>
#include <array>
#include <cassert>
#include <span>
#include <utility>
#include <vector>

namespace
{
    struct Position final
    {
        int value{};
        lux::simulation::ecs::Entity target{lux::simulation::ecs::NullEntity};
    };

    struct DerivedCache final
    {
        int value{};
    };

    struct UnknownStorage final
    {
        int value{};
    };
}

int
main()
{
    const auto position_schema = lux::simulation::ecs::makeComponentSchema<Position>(
        lux::simulation::ecs::componentSchemaId("test.position"),
        1U,
        lux::simulation::ecs::EComponentSnapshotPolicy::COPY,
        {},
        {},
        lux::simulation::ecs::EComponentSemanticKind::DOMAIN_CONTRACT,
        true
    );
    const auto cache_schema = lux::simulation::ecs::makeComponentSchema<DerivedCache>(
        lux::simulation::ecs::componentSchemaId("test.cache"),
        1,
        lux::simulation::ecs::EComponentSnapshotPolicy::REBUILD,
        {},
        {},
        lux::simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED,
        false
    );
    auto schemas = lux::simulation::ecs::ComponentSchemaSet::build({position_schema, cache_schema});
    assert(schemas);
    const std::array snapshot_bindings{lux::simulation::ecs::bindComponentSnapshot<Position>(position_schema)};
    const lux::simulation::ecs::ComponentSnapshotContribution snapshot_contribution{{}, snapshot_bindings};
    auto snapshot_components =
        lux::simulation::ecs::ComponentSnapshotSet::build(*schemas, std::span(&snapshot_contribution, 1U));
    assert(snapshot_components);

    lux::simulation::ecs::Registry source;
    const auto first = source.create();
    const auto removed = source.create();
    const auto third = source.create();
    source.emplace<Position>(first, 7, third);
    source.emplace<Position>(third, 9, first);
    source.emplace<DerivedCache>(first, 99);
    source.destroy(removed);
    std::vector<lux::simulation::ecs::Entity> bulk;
    bulk.reserve(10'000);
    for (int index{}; index < 10'000; ++index
    )
    {
        const auto entity = source.create();
        source.emplace<Position>(entity, index, first);
        bulk.push_back(entity);
    }
    for (std::size_t index{}; index < bulk.size(); index += 3)
        source.destroy(bulk[index]);

    lux::simulation::ecs::detail::ComponentSnapshotTestStats::reset();
    auto snapshot = lux::simulation::ecs::EcsSnapshot::capture(source, *snapshot_components);
    assert(snapshot);
    assert(lux::simulation::ecs::detail::ComponentSnapshotTestStats::clone_calls == 1U);
    assert(lux::simulation::ecs::detail::ComponentSnapshotTestStats::storage_lookups == 1U);
    auto instance = snapshot->instantiate();
    assert(instance);
    assert((*instance)->valid(first));
    assert((*instance)->valid(third));
    assert(!(*instance)->valid(removed));
    assert((*instance)->get<Position>(first).target == third);
    assert((*instance)->try_get<DerivedCache>(first) == nullptr);
    for (std::size_t index{}; index < bulk.size(); ++index)
    {
        assert((*instance)->valid(bulk[index]) == (index % 3 != 0));
        if (index % 3 != 0)
            assert((*instance)->get<Position>(bulk[index]).value == index);
    }

    std::vector<lux::simulation::ecs::Entity> source_next;
    source_next.reserve(100);
    for (int index{}; index < 100; ++index)
        source_next.push_back(source.create());

    for (int index{}; index < 100; ++index)
        assert(source_next[index] == (*instance)->create());

    lux::simulation::ecs::Registry restored;
    const auto unrelated = restored.create();
    restored.emplace<DerivedCache>(unrelated, 1);
    assert(snapshot->restore(restored));
    assert(restored.valid(first));
    assert(restored.get<Position>(third).target == first);
    assert(restored.try_get<DerivedCache>(first) == nullptr);

    lux::simulation::ecs::Registry invalid;
    const auto unknown = invalid.create();
    invalid.emplace<UnknownStorage>(unknown, 3);
    const auto invalid_snapshot = lux::simulation::ecs::EcsSnapshot::capture(invalid, *snapshot_components);
    assert(!invalid_snapshot);
    assert(invalid_snapshot.error().code == lux::simulation::ecs::ESnapshotError::UNKNOWN_COMPONENT_STORAGE);
}
