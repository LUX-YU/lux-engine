#include <lux/engine/editor/scene/AuthoringFacts.hpp>
#include <algorithm>

namespace lux::editor::scene
{
    bool isAuthorComponent(const simulation::ecs::ComponentSchema& schema) noexcept
    {
        namespace ecs = simulation::ecs;
        return schema.snapshot == ecs::EComponentSnapshotPolicy::COPY &&
               schema.semantic_kind != ecs::EComponentSemanticKind::RUNTIME_DERIVED &&
               schema.decode_value && schema.capture;
    }
    bool declaresAuthorSchema(std::span<const world::WorldDataSchemaId> schemas, std::string_view name) noexcept
    {
        return std::ranges::find(schemas, name, &world::WorldDataSchemaId::name) != schemas.end();
    }
    AuthoringFacts authoringFacts(
        const world::WorldDescription& world, const simulation::ecs::ComponentSchemaSet& schemas,
        sessions::ContentStamp stamp, bool available
    ) noexcept
    {
        return {
            stamp, world.schemas(), schemas, world.partitioner().id.name,
            world.partitioner().version, !world.partitionIndexes().empty(), available
        };
    }
    ApplicabilityResult queryApplicability(const AuthoringFacts& facts, const ApplicabilityRequirements& needs)
    {
        const auto denied = [&](EApplicabilityReason reason, std::string_view subject = {}) {
            return ApplicabilityResult{EApplicability::NOT_APPLICABLE, reason, facts.based_on, std::string{subject}};
        };
        for (const auto name : needs.schemas)
        {
            if (!declaresAuthorSchema(facts.schemas, name))
                return denied(EApplicabilityReason::UNDECLARED_SCHEMA, name);
            const auto* schema = facts.registrations.find(simulation::ecs::componentSchemaId(name));
            if (!schema)
                return denied(EApplicabilityReason::MISSING_PROVIDER, name);
            if (!isAuthorComponent(*schema))
                return denied(EApplicabilityReason::NON_AUTHOR_VALUE, name);
            if (needs.default_values && !schema->create)
                return denied(EApplicabilityReason::NO_DEFAULT_VALUE, name);
        }
        const bool is_partition_mismatch = !needs.partition.empty() &&
            (facts.partition != needs.partition || facts.partition_version != needs.partition_version);
        if (is_partition_mismatch)
            return denied(EApplicabilityReason::PARTITION_MISMATCH, needs.partition);
        if (needs.edit_content && facts.indexed)
            return denied(EApplicabilityReason::INDEX_REBUILD_REQUIRED);
        if (!facts.available)
            return {EApplicability::TEMPORARILY_UNAVAILABLE, EApplicabilityReason::ADMISSION, facts.based_on, {}};
        return {EApplicability::SUPPORTED, EApplicabilityReason::NONE, facts.based_on, {}};
    }
}
