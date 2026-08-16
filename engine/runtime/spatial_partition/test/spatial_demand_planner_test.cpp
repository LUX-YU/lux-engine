#include <lux/engine/runtime/spatial_partition/SpatialDemandPlanner.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{
    uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    lux::entity_scene::EntitySectionRecord record(
        const char* id,
        std::uint64_t decoded_bytes,
        std::uint32_t entity_count,
        std::string channel = "org.lux.test.visible")
    {
        lux::entity_scene::EntitySectionRecord value;
        value.id = lux::entity_scene::EntitySectionId{uuid(id)};
        value.source = lux::entity_scene::StoredSectionSource{
            "/Game/Sections/Test_lxes"};
        value.content_digest[0] = std::byte{1u};
        value.encoded_bytes = decoded_bytes;
        value.decoded_bytes = decoded_bytes;
        value.entity_count = entity_count;
        value.demand_channels.emplace_back(std::move(channel));
        return value;
    }

    lux::runtime::spatial_partition::SpatialDemandSourceUpdate update(
        std::string source,
        std::uint64_t generation,
        std::initializer_list<
            lux::runtime::spatial_partition::SpatialDemandEntry> demands,
        std::string channel = "org.lux.test.visible")
    {
        return {
            lux::runtime::spatial_partition::SpatialDemandSourceId{
                std::move(source)},
            generation,
            lux::entity_scene::DemandChannelId{std::move(channel)},
            {demands}};
    }

    lux::runtime::entity_scene::EntitySceneCatalog catalog(
        std::vector<lux::entity_scene::EntitySectionRecord> records)
    {
        std::sort(
            records.begin(), records.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.id.value() < rhs.id.value();
            });
        lux::entity_scene::EntitySceneManifest manifest;
        manifest.id = lux::entity_scene::EntitySceneId{uuid(
            "80000000-0000-4000-8000-000000000001")};
        manifest.sections = std::move(records);
        auto result = lux::runtime::entity_scene::EntitySceneCatalog::create(
            std::move(manifest));
        assert(result);
        return std::move(*result);
    }
}

int main()
{
    namespace partition = lux::runtime::spatial_partition;
    using lux::entity_scene::EntitySectionId;

    auto first = record(
        "81000000-0000-4000-8000-000000000001", 60u, 6u);
    auto second = record(
        "81000000-0000-4000-8000-000000000002", 70u, 7u);
    auto third = record(
        "81000000-0000-4000-8000-000000000003", 40u, 4u);
    auto dependency = record(
        "81000000-0000-4000-8000-000000000004", 10u, 1u);
    dependency.dependencies.push_back(first.id);

    const auto first_id = first.id;
    const auto second_id = second.id;
    const auto third_id = third.id;
    const auto dependency_id = dependency.id;
    auto scene_catalog = catalog({first, second, third, dependency});
    partition::EntitySectionRecordStore store{scene_catalog};
    auto planner = partition::SpatialDemandPlanner::create(
        std::move(store),
        partition::SpatialPartitionBudget{150u, 15u});
    assert(planner);

    auto first_plan = planner->prepareReplace(update(
        "org.lux.test.source_a",
        1u,
        {{first_id, 4u}, {second_id, 2u}}));
    assert(first_plan);
    assert(first_plan->snapshot().resident_sections == 2u);
    assert(first_plan->snapshot().decoded_bytes == 130u);
    assert(planner->commit(std::move(*first_plan)));

    auto overlap = planner->prepareReplace(update(
        "org.lux.test.source_b", 1u, {{first_id, 9u}}));
    assert(overlap);
    assert(overlap->snapshot().source_count == 2u);
    assert(overlap->snapshot().resident_sections == 2u);
    assert(overlap->snapshot().source_references == 3u);
    assert(overlap->residents()[0].source_references == 2u);
    assert(overlap->residents()[0].priority == 9u);
    assert(planner->commit(std::move(*overlap)));
    const auto stable = planner->snapshot();

    // The prospective union exceeds the byte budget. The old source and
    // generation remain authoritative because no commit occurred.
    auto over_budget = planner->prepareReplace(update(
        "org.lux.test.source_b",
        2u,
        {{first_id, 1u}, {third_id, 1u}}));
    assert(!over_budget);
    assert(over_budget.error().code ==
        partition::ESpatialPartitionError::
            DECODED_BYTE_BUDGET_EXCEEDED);
    assert(planner->snapshot().revision == stable.revision);
    assert(planner->snapshot().decoded_bytes == stable.decoded_bytes);

    auto mismatch = planner->prepareReplace(update(
        "org.lux.test.source_b",
        2u,
        {{third_id, 1u}},
        "org.lux.test.other"));
    assert(!mismatch);
    assert(mismatch.error().code ==
        partition::ESpatialPartitionError::CHANNEL_MISMATCH);
    assert(planner->snapshot().revision == stable.revision);

    // The planner expands a hard dependency closure and accounts one source
    // reference for each resident Section. The live loader consumes this
    // exact closure in topological order; dependency metadata is never
    // stripped to fake success.
    auto dependency_plan = planner->prepareReplace(update(
        "org.lux.test.source_b", 2u, {{dependency_id, 1u}}));
    assert(dependency_plan);
    // Source A still contributes first+second while source B now contributes
    // dependency+first. The unique prospective closure is therefore all
    // three records; the shared first Section is counted once resident and
    // twice in source references.
    assert(dependency_plan->snapshot().resident_sections == 3u);
    assert(dependency_plan->snapshot().source_references == 4u);
    assert(std::any_of(
        dependency_plan->residents().begin(),
        dependency_plan->residents().end(),
        [&dependency_id](const auto& resident)
        {
            return resident.record.id == dependency_id;
        }));
    assert(planner->snapshot().revision == stable.revision);

    auto stale_generation = planner->prepareReplace(update(
        "org.lux.test.source_b", 1u, {{first_id, 1u}}));
    assert(!stale_generation);
    assert(stale_generation.error().code ==
        partition::ESpatialPartitionError::STALE_SOURCE_GENERATION);

    // Prepared plans are revision-bound. A second prospective plan cannot
    // overwrite a mutation committed after it was prepared.
    auto source_a_v2 = planner->prepareReplace(update(
        "org.lux.test.source_a", 2u, {{first_id, 3u}}));
    auto source_b_v2 = planner->prepareReplace(update(
        "org.lux.test.source_b", 2u, {{first_id, 5u}}));
    assert(source_a_v2 && source_b_v2);
    assert(planner->commit(std::move(*source_a_v2)));
    auto stale_plan = planner->commit(std::move(*source_b_v2));
    assert(!stale_plan);
    assert(stale_plan.error().code ==
        partition::ESpatialPartitionError::PLAN_STALE);
    assert(planner->snapshot().resident_sections == 1u);
    assert(planner->snapshot().source_references == 2u);

    partition::SpatialDemandSourceId source_a{
        "org.lux.test.source_a"};
    auto remove_a = planner->prepareRemove(source_a, 2u);
    assert(remove_a);
    assert(planner->commit(std::move(*remove_a)));
    assert(planner->snapshot().resident_sections == 1u);
    assert(planner->snapshot().source_references == 1u);

    // Procedural records live with the source transaction instead of an
    // ever-growing global index. Another source may overlap only with an
    // identical record; a conflicting digest fails without changing state.
    auto generated = record(
        "81000000-0000-4000-8000-000000000005", 12u, 1u);
    generated.source = lux::entity_scene::GeneratedSectionSource{
        lux::entity_scene::SectionGeneratorId{"org.lux.test.generator"},
        99u,
        {std::byte{1u}, std::byte{2u}}};
    const auto generated_id = generated.id;
    auto unused_generated = generated;
    unused_generated.id = EntitySectionId{uuid(
        "81000000-0000-4000-8000-000000000006")};
    auto unused_update = update(
        "org.lux.test.generated_unused", 1u, {{generated_id, 7u}});
    unused_update.records.push_back(generated);
    unused_update.records.push_back(std::move(unused_generated));
    auto unused = planner->prepareReplace(std::move(unused_update));
    assert(!unused);
    assert(unused.error().code ==
        partition::ESpatialPartitionError::INVALID_DYNAMIC_RECORD);

    auto generated_update = update(
        "org.lux.test.generated", 1u, {{generated_id, 7u}});
    generated_update.records.push_back(generated);
    auto generated_plan = planner->prepareReplace(
        std::move(generated_update));
    assert(generated_plan);
    assert(generated_plan->snapshot().dynamic_records == 1u);
    assert(planner->commit(std::move(*generated_plan)));

    // Source-owned metadata cannot be borrowed implicitly by another source.
    // Otherwise removing the metadata owner would make the borrower dangle.
    auto borrowed_update = update(
        "org.lux.test.generated_borrow", 1u, {{generated_id, 3u}});
    const auto before_borrow = planner->snapshot();
    auto borrowed = planner->prepareReplace(std::move(borrowed_update));
    assert(!borrowed);
    assert(borrowed.error().code ==
        partition::ESpatialPartitionError::SECTION_NOT_FOUND);
    assert(planner->snapshot().revision == before_borrow.revision);

    // Overlap is explicit: each source supplies the same canonical record,
    // while the aggregate stores one resident copy and two references.
    auto exact_overlap_update = update(
        "org.lux.test.generated_overlap", 1u, {{generated_id, 5u}});
    exact_overlap_update.records.push_back(generated);
    auto exact_overlap = planner->prepareReplace(
        std::move(exact_overlap_update));
    assert(exact_overlap);
    assert(exact_overlap->snapshot().dynamic_records == 1u);
    assert(planner->commit(std::move(*exact_overlap)));

    auto conflicting = generated;
    conflicting.content_digest[0] = std::byte{2u};
    auto conflict_update = update(
        "org.lux.test.generated_conflict", 1u, {{generated_id, 5u}});
    conflict_update.records.push_back(std::move(conflicting));
    const auto before_conflict = planner->snapshot();
    auto conflict = planner->prepareReplace(std::move(conflict_update));
    assert(!conflict);
    assert(conflict.error().code ==
        partition::ESpatialPartitionError::DYNAMIC_RECORD_CONFLICT);
    assert(planner->snapshot().revision == before_conflict.revision);

    partition::SpatialDemandSourceId generated_source{
        "org.lux.test.generated"};
    auto remove_generated = planner->prepareRemove(generated_source, 1u);
    assert(remove_generated);
    assert(remove_generated->snapshot().dynamic_records == 1u);
    assert(planner->commit(std::move(*remove_generated)));

    partition::SpatialDemandSourceId generated_overlap_source{
        "org.lux.test.generated_overlap"};
    auto remove_generated_overlap = planner->prepareRemove(
        generated_overlap_source, 1u);
    assert(remove_generated_overlap);
    assert(remove_generated_overlap->snapshot().dynamic_records == 0u);
    assert(planner->commit(std::move(*remove_generated_overlap)));

    partition::SpatialDemandSourceId source_b{
        "org.lux.test.source_b"};
    auto stale_remove = planner->prepareRemove(source_b, 2u);
    assert(!stale_remove);
    assert(stale_remove.error().code ==
        partition::ESpatialPartitionError::STALE_SOURCE_GENERATION);
    auto remove_b = planner->prepareRemove(source_b, 1u);
    assert(remove_b);
    assert(planner->commit(std::move(*remove_b)));
    assert(planner->snapshot().source_count == 0u);
    assert(planner->snapshot().resident_sections == 0u);
    assert(planner->snapshot().decoded_bytes == 0u);
    return 0;
}
