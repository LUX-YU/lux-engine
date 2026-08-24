#include <lux/engine/ecs/entity_scene/residency/EntitySectionResidencySystem.hpp>

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace lux::ecs::entity_scene::residency
{
    struct EntitySectionResidencySystem::ResidentTicket final
    {
        lux::ecs::scene_format::EntitySectionId section;
        lux::ecs::entity_scene::EntitySectionTicket ticket;
    };

    namespace
    {
        [[nodiscard]] bool sectionLess(
            const lux::ecs::scene_format::EntitySectionId& lhs,
            const lux::ecs::scene_format::EntitySectionId& rhs) noexcept
        {
            return lhs.value() < rhs.value();
        }
    }

    EntitySectionResidencySystem::EntitySectionResidencySystem(
        lux::ecs::entity_scene::EntitySectionClient client,
        SectionResidencyPlanner planner) noexcept
        : client_(std::move(client)),
          planner_(std::move(planner)),
          owner_thread_(std::this_thread::get_id())
    {}

    EntitySectionResidencySystem::~EntitySectionResidencySystem()
    {
        requireOwnerThread();
    }

    void EntitySectionResidencySystem::requireOwnerThread() const noexcept
    {
        if (std::this_thread::get_id() != owner_thread_)
            std::abort();
    }

    void EntitySectionResidencySystem::recordFailure(
        ESectionResidencyError error) noexcept
    {
        ++rejected_transactions_;
        if (error == ESectionResidencyError::
                DECODED_BYTE_BUDGET_EXCEEDED ||
            error == ESectionResidencyError::ENTITY_BUDGET_EXCEEDED ||
            error == ESectionResidencyError::BUDGET_OVERFLOW)
        {
            ++budget_rejections_;
        }
        if (error == ESectionResidencyError::STALE_SOURCE_GENERATION)
            ++stale_generation_rejections_;
    }

    SectionResidencyExp<void>
    EntitySectionResidencySystem::replaceDemandSource(
        SectionDemandSourceUpdate update)
    {
        requireOwnerThread();
        if (!added_)
        {
            const auto error = binding_mismatch_
                ? ESectionResidencyError::LOADER_REGISTRY_MISMATCH
                : ESectionResidencyError::OWNER_NOT_ADDED;
            recordFailure(error);
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = error,
                .source = std::move(update.source)});
        }
        auto plan = planner_.prepareReplace(std::move(update));
        if (!plan)
        {
            recordFailure(plan.error().code);
            return lux::cxx::unexpected(std::move(plan.error()));
        }
        return apply(std::move(*plan), false);
    }

    SectionResidencyExp<void>
    EntitySectionResidencySystem::removeDemandSource(
        const SectionDemandSourceId& source,
        std::uint64_t generation)
    {
        requireOwnerThread();
        if (!added_)
        {
            const auto error = binding_mismatch_
                ? ESectionResidencyError::LOADER_REGISTRY_MISMATCH
                : ESectionResidencyError::OWNER_NOT_ADDED;
            recordFailure(error);
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = error,
                .source = source});
        }
        auto plan = planner_.prepareRemove(source, generation);
        if (!plan)
        {
            recordFailure(plan.error().code);
            return lux::cxx::unexpected(std::move(plan.error()));
        }
        return apply(std::move(*plan), true);
    }

    SectionResidencyExp<void>
    EntitySectionResidencySystem::apply(SectionResidencyPlan plan, bool removal)
    {
        std::vector<SectionResidentDemand> desired{
            plan.residents().begin(), plan.residents().end()};
        std::sort(
            desired.begin(), desired.end(),
            [](const SectionResidentDemand& lhs,
               const SectionResidentDemand& rhs)
            {
                return sectionLess(lhs.record.id, rhs.record.id);
            });
        std::vector<ResidentTicket> acquired;
        acquired.reserve(desired.size());
        std::vector<bool> retained_sections(desired.size(), false);
        for (const auto& resident : desired)
        {
            const auto retained_ticket = std::lower_bound(
                resident_tickets_.begin(),
                resident_tickets_.end(),
                resident.record.id,
                [](const ResidentTicket& lhs, const auto& rhs)
                {
                    return sectionLess(lhs.section, rhs);
                });
            if (retained_ticket != resident_tickets_.end() &&
                retained_ticket->section == resident.record.id)
            {
                const auto index = static_cast<std::size_t>(
                    &resident - desired.data());
                retained_sections[index] = true;
                continue;
            }
        }

        // Acquire a closure in dependency order. EntitySectionLoaderSystem
        // pins dependencies by their already-admitted slot, so ordering by
        // priority/UUID alone would make correctness depend on UUID layout.
        std::vector<const SectionResidentDemand*> roots;
        roots.reserve(desired.size());
        for (const auto& resident : desired)
            roots.push_back(&resident);
        std::sort(
            roots.begin(), roots.end(),
            [](const auto* lhs, const auto* rhs)
            {
                if (lhs->priority != rhs->priority)
                    return lhs->priority > rhs->priority;
                return sectionLess(lhs->record.id, rhs->record.id);
            });

        std::vector<std::uint8_t> visits(desired.size(), 0u);
        std::vector<const SectionResidentDemand*> missing;
        missing.reserve(desired.size());
        auto appendWithDependencies = [&](auto&& self,
                                          const SectionResidentDemand& value)
            -> bool
        {
            const auto index = static_cast<std::size_t>(
                &value - desired.data());
            if (visits[index] == 2u)
                return true;
            if (visits[index] == 1u)
                return false;
            visits[index] = 1u;
            for (const auto dependency : value.record.dependencies)
            {
                const auto found = std::lower_bound(
                    desired.begin(), desired.end(), dependency,
                    [](const SectionResidentDemand& lhs, const auto& rhs)
                    {
                        return sectionLess(lhs.record.id, rhs);
                    });
                if (found == desired.end() || found->record.id != dependency ||
                    !self(self, *found))
                {
                    return false;
                }
            }
            visits[index] = 2u;
            if (!retained_sections[index])
                missing.push_back(&value);
            return true;
        };
        for (const auto* root : roots)
        {
            if (!appendWithDependencies(appendWithDependencies, *root))
            {
                recordFailure(ESectionResidencyError::INVALID_DEPENDENCY);
                return lux::cxx::unexpected(SectionResidencyFailure{
                    .code = ESectionResidencyError::INVALID_DEPENDENCY,
                    .section = root->record.id});
            }
        }
        // Closing the loader stops new admission, but existing tickets remain
        // valid owners and must still be releasable.  A shrink/remove plan
        // with no missing Section therefore commits after admission closes;
        // only a plan which actually needs a fresh acquire requires a live
        // client.  Otherwise SpatialInterest close can never clear its final
        // demand source and will report owner work forever.
        if (!missing.empty() && !client_)
        {
            recordFailure(ESectionResidencyError::LOADER_UNAVAILABLE);
            return lux::cxx::unexpected(SectionResidencyFailure{
                .code = ESectionResidencyError::LOADER_UNAVAILABLE});
        }
        for (const auto* resident : missing)
        {
            auto ticket = client_.acquire(resident->record);
            if (!ticket)
            {
                recordFailure(
                    ESectionResidencyError::SECTION_ACQUIRE_FAILED);
                return lux::cxx::unexpected(SectionResidencyFailure{
                    .code = ESectionResidencyError::SECTION_ACQUIRE_FAILED,
                    .section = resident->record.id,
                    .loader_error = ticket.error()});
            }
            acquired.push_back(ResidentTicket{
                resident->record.id, std::move(*ticket)});
        }
        std::sort(
            acquired.begin(), acquired.end(),
            [](const ResidentTicket& lhs, const ResidentTicket& rhs)
            {
                return sectionLess(lhs.section, rhs.section);
            });

        auto committed = planner_.commit(std::move(plan));
        if (!committed)
        {
            recordFailure(committed.error().code);
            return lux::cxx::unexpected(std::move(committed.error()));
        }

        std::vector<ResidentTicket> next;
        next.reserve(desired.size());
        for (const auto& resident : desired)
        {
            auto retained = std::lower_bound(
                resident_tickets_.begin(),
                resident_tickets_.end(),
                resident.record.id,
                [](const ResidentTicket& lhs, const auto& rhs)
                {
                    return sectionLess(lhs.section, rhs);
                });
            if (retained != resident_tickets_.end() &&
                retained->section == resident.record.id)
            {
                next.push_back(ResidentTicket{
                    retained->section, std::move(retained->ticket)});
                continue;
            }
            auto fresh = std::lower_bound(
                acquired.begin(),
                acquired.end(),
                resident.record.id,
                [](const ResidentTicket& lhs, const auto& rhs)
                {
                    return sectionLess(lhs.section, rhs);
                });
            if (fresh == acquired.end() ||
                fresh->section != resident.record.id)
            {
                std::abort();
            }
            next.push_back(ResidentTicket{
                fresh->section, std::move(fresh->ticket)});
        }
        resident_tickets_.swap(next);
        if (removal)
            ++committed_removals_;
        else
            ++committed_replacements_;
        return {};
    }

    EntitySectionResidencySnapshot EntitySectionResidencySystem::snapshot() const noexcept
    {
        requireOwnerThread();
        EntitySectionResidencySnapshot result{
            .demand = planner_.snapshot(),
            .loader_tickets = resident_tickets_.size(),
            .loader_binding_valid = added_,
            .committed_replacements = committed_replacements_,
            .committed_removals = committed_removals_,
            .rejected_transactions = rejected_transactions_,
            .budget_rejections = budget_rejections_,
            .stale_generation_rejections =
                stale_generation_rejections_};
        for (const auto& resident : resident_tickets_)
        {
            using lux::ecs::entity_scene::EEntitySectionState;
            switch (resident.ticket.state())
            {
            case EEntitySectionState::WAITING_ADMISSION:
            case EEntitySectionState::WAITING_BACKGROUND:
                ++result.waiting_sections;
                break;
            case EEntitySectionState::STAGING:
            case EEntitySectionState::ARMED:
                ++result.staging_sections;
                break;
            case EEntitySectionState::ACTIVE:
            case EEntitySectionState::DEACTIVATE_QUEUED:
                ++result.active_sections;
                break;
            case EEntitySectionState::FAILED:
                ++result.failed_sections;
                break;
            default:
                break;
            }
        }
        return result;
    }

    void EntitySectionResidencySystem::onAdded(
        const lux::ecs::SystemSetupContext& setup)
    {
        requireOwnerThread();
        added_ = client_.boundTo(setup.registry());
        binding_mismatch_ = !added_;
    }

    void EntitySectionResidencySystem::update(
        const lux::ecs::SystemUpdateContext&)
    {
        requireOwnerThread();
    }

    std::span<const lux::ecs::ISystem::Type>
    EntitySectionResidencySystem::prerequisites() const noexcept
    {
        static constexpr Type prerequisites[]{
            lux::ecs::systemType<
                lux::ecs::entity_scene::EntitySectionLoaderSystem>()};
        return prerequisites;
    }

    std::span<const lux::ecs::ISystem::Type>
    EntitySectionResidencySystem::runsAfter() const noexcept
    {
        return prerequisites();
    }
}
