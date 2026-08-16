#include <lux/engine/runtime/entity_scene/EntityBatchMaterializer.hpp>

#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/RegistryStorageCapacity.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp>

#include "EntityBatchInternal.hpp"

#include <cstddef>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lux::runtime::entity_scene
{
    struct SectionCommitReceipt::Impl final
    {
        lux::entity_scene::EntitySectionId section;
        std::uint64_t generation{0u};
        std::vector<entt::entity> entities;
        std::vector<ContentBlobLease> blob_leases;
        lux::ecs::PersistentEntityIdClaim persistent_claim;
        std::vector<std::uint32_t> persistent_ordinals;
        std::vector<entt::entity> persistent_entities;
    };

    SectionCommitReceipt::SectionCommitReceipt()
        : impl_(std::make_unique<Impl>())
    {}

    SectionCommitReceipt::~SectionCommitReceipt() = default;

    const lux::entity_scene::EntitySectionId& SectionCommitReceipt::section()
        const noexcept
    {
        return impl_->section;
    }

    std::uint64_t SectionCommitReceipt::generation() const noexcept
    {
        return impl_->generation;
    }

    std::span<const entt::entity> SectionCommitReceipt::entities()
        const noexcept
    {
        return impl_->entities;
    }

    struct EntityBatchMaterializer::Impl final
    {
        explicit Impl(
            lux::ecs::PersistentEntityIndex& persistent_entities_value)
            : persistent_entities(persistent_entities_value)
        {}

        enum class ESlotState : std::uint8_t
        {
            EMPTY,
            ARMED,
            ACTIVE
        };

        struct Slot final
        {
            ESlotState state{ESlotState::EMPTY};
            std::unique_ptr<SectionCommitReceipt> receipt;
            lux::meta::RegistryPublicationReservation
                publication_reservation;
            std::size_t reserved_entities{0u};
            std::size_t reserved_parents{0u};
            std::size_t reserved_persistent_entities{0u};
            std::vector<std::pair<std::string, std::size_t>>
                reserved_components;
        };

        void releaseArmedReservation(Slot& slot) noexcept
        {
            if (slot.reserved_entities > armed_entities ||
                slot.reserved_parents > armed_parents ||
                slot.reserved_persistent_entities >
                    armed_persistent_entities)
            {
                std::abort();
            }
            armed_entities -= slot.reserved_entities;
            armed_parents -= slot.reserved_parents;
            armed_persistent_entities -=
                slot.reserved_persistent_entities;
            for (const auto& [schema, count] : slot.reserved_components)
            {
                const auto found = armed_components.find(schema);
                if (found == armed_components.end() ||
                    found->second < count)
                {
                    std::abort();
                }
                found->second -= count;
                if (found->second == 0u)
                    armed_components.erase(found);
            }
            slot.reserved_entities = 0u;
            slot.reserved_parents = 0u;
            slot.reserved_persistent_entities = 0u;
            slot.reserved_components.clear();
        }

        lux::meta::EntityRegistry* registry{nullptr};
        lux::ecs::PersistentEntityIndex& persistent_entities;
        std::map<uuids::uuid, Slot> slots;
        std::size_t armed_entities{0u};
        std::size_t armed_parents{0u};
        std::size_t armed_persistent_entities{0u};
        std::map<std::string, std::size_t> armed_components;
        std::uint64_t published_batches{0u};
        std::uint64_t deactivated_batches{0u};
        std::uint64_t already_destroyed_entities{0u};
        std::uint64_t stale_rejections{0u};
    };

    namespace
    {
        [[nodiscard]] EntityBatchFailure materializerFailure(
            EEntityBatchError error,
            const lux::entity_scene::EntitySectionId& section,
            std::uint64_t generation,
            std::string detail)
        {
            return detail::makeFailure(
                error, section, generation, std::move(detail));
        }

        [[noreturn]] void publicationInvariantFailed() noexcept
        {
            std::abort();
        }

        [[nodiscard]] bool addSparsePublicationBudget(
            std::size_t writes,
            std::size_t& total) noexcept
        {
            const auto budget =
                lux::ecs::registrySparsePublicationBytes(writes);
            return budget && lux::ecs::addRegistryPublicationBytes(
                total, *budget, total);
        }
    }

    EntityBatchMaterializer::EntityBatchMaterializer(
        lux::ecs::PersistentEntityIndex& persistent_entities)
        : impl_(std::make_unique<Impl>(persistent_entities))
    {}

    EntityBatchMaterializer::~EntityBatchMaterializer() = default;

    lux::cxx::expected<void, EntityBatchFailure>
    EntityBatchMaterializer::arm(
        PreparedEntityBatch& batch,
        lux::meta::EntityRegistry& live) noexcept
    {
        if (!batch.impl_ ||
            batch.impl_->state != EPreparedEntityBatchState::READY)
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::NOT_READY,
                batch.section(),
                batch.generation(),
                "EntitySection is not ready for publication"));
        }
        auto& prepared = *batch.impl_;
        const auto& image = prepared.decoded.image_;
        const auto& section = image.section;
        const auto generation = prepared.decoded.generation();

        if (impl_->registry && impl_->registry != &live)
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::REGISTRY_MISMATCH,
                section,
                generation,
                "EntityBatchMaterializer is bound to another registry"));
        }
        if (!impl_->persistent_entities.boundTo(live))
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::REGISTRY_MISMATCH,
                section,
                generation,
                "PersistentEntityIndex is bound to another registry"));
        }

        if (impl_->slots.contains(section.value()))
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::SECTION_ALREADY_ACTIVE,
                section,
                generation,
                "EntitySection already has an armed or active generation"));
        }
        std::size_t reserved_entities = 0u;
        if (!lux::ecs::checkedAdditionalCapacity(
                impl_->armed_entities,
                image.entities.size(),
                reserved_entities) ||
            !lux::ecs::reserveAdditionalStorageCapacity(
                live.storage<entt::entity>(), reserved_entities))
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::CAPACITY_OVERFLOW,
                section,
                generation,
                "entity storage capacity reservation failed"));
        }
        for (std::size_t schema = 0u;
             schema < prepared.schemas.size();
             ++schema)
        {
            if (prepared.schema_counts[schema] != 0u)
            {
                const auto& schema_name =
                    prepared.schemas[schema].schema_id.name;
                const auto pending = impl_->armed_components.contains(
                                         schema_name)
                    ? impl_->armed_components.at(schema_name)
                    : 0u;
                std::size_t reserved_components = 0u;
                if (!lux::ecs::checkedAdditionalCapacity(
                        pending,
                        prepared.schema_counts[schema],
                        reserved_components))
                {
                    return lux::cxx::unexpected(materializerFailure(
                        EEntityBatchError::CAPACITY_OVERFLOW,
                        section,
                        generation,
                        "component storage capacity reservation overflow"));
                }
                prepared.schemas[schema].operations.reserve(
                    live, reserved_components);
            }
        }
        std::size_t reserved_parents = 0u;
        if (!lux::ecs::checkedAdditionalCapacity(
                impl_->armed_parents,
                image.parents.size(),
                reserved_parents) ||
            !lux::ecs::reserveAdditionalStorageCapacity(
                live.storage<lux::ecs::ParentComponent>(),
                reserved_parents))
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::CAPACITY_OVERFLOW,
                section,
                generation,
                "Parent storage capacity reservation failed"));
        }

        std::vector<lux::entity_scene::PersistentEntityId> persistent_ids;
        std::vector<std::uint32_t> persistent_ordinals;
        persistent_ids.reserve(image.entities.size());
        persistent_ordinals.reserve(image.entities.size());
        for (std::uint32_t ordinal = 0u;
             ordinal < image.entities.size();
             ++ordinal)
        {
            if (!image.entities[ordinal].persistent_id)
                continue;
            persistent_ids.push_back(*image.entities[ordinal].persistent_id);
            persistent_ordinals.push_back(ordinal);
        }
        const auto persistent_count = persistent_ids.size();
        if (persistent_count != 0u)
        {
            std::size_t reserved_persistent_entities = 0u;
            if (!lux::ecs::checkedAdditionalCapacity(
                    impl_->armed_persistent_entities,
                    persistent_count,
                    reserved_persistent_entities) ||
                !lux::ecs::reserveAdditionalStorageCapacity(
                    live.storage<lux::ecs::PersistentEntityIdComponent>(),
                    reserved_persistent_entities))
            {
                return lux::cxx::unexpected(materializerFailure(
                    EEntityBatchError::CAPACITY_OVERFLOW,
                    section,
                    generation,
                    "persistent ID storage capacity reservation failed"));
            }
        }
        if (!image.parents.empty())
        {
            auto& hierarchy = lux::ecs::ensureHierarchyIndex(live);
            if (!hierarchy.reserveForAdditionalEdges(reserved_parents))
            {
                return lux::cxx::unexpected(materializerFailure(
                    EEntityBatchError::CAPACITY_OVERFLOW,
                    section,
                    generation,
                    "hierarchy index reservation failed"));
            }
        }

        // Packed arrays and signal-side indices above are pre-grown against
        // every batch which is already ARMED. Sparse pages cannot be forced
        // into existence through EnTT's reserve() surface, so account for
        // every target storage independently and give this batch its own
        // registry-owned publication block. Separate blocks are what make
        // two same-barrier ARMED batches composable without stealing one
        // another's capacity.
        std::size_t hierarchy_membership_writes = 0u;
        std::size_t publication_bytes = 0u;
        if (!lux::ecs::checkedAdditionalCapacity(
                image.parents.size(),
                image.parents.size(),
                hierarchy_membership_writes) ||
            !addSparsePublicationBudget(
                image.entities.size(), publication_bytes) ||
            !addSparsePublicationBudget(
                image.parents.size(), publication_bytes) ||
            !addSparsePublicationBudget(
                hierarchy_membership_writes, publication_bytes) ||
            !addSparsePublicationBudget(
                persistent_count, publication_bytes))
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::CAPACITY_OVERFLOW,
                section,
                generation,
                "registry sparse publication budget overflow"));
        }
        for (const auto count : prepared.schema_counts)
        {
            if (!addSparsePublicationBudget(count, publication_bytes))
            {
                return lux::cxx::unexpected(materializerFailure(
                    EEntityBatchError::CAPACITY_OVERFLOW,
                    section,
                    generation,
                    "component sparse publication budget overflow"));
            }
        }
        auto publication_reservation =
            live.reservePublication(publication_bytes);
        if (!publication_reservation)
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::CAPACITY_OVERFLOW,
                section,
                generation,
                "registry publication memory reservation failed"));
        }

        auto receipt = std::unique_ptr<SectionCommitReceipt>{
            new SectionCommitReceipt};
        receipt->impl_->section = section;
        receipt->impl_->generation = generation;
        receipt->impl_->entities.resize(
            prepared.publication_entities.size());
        receipt->impl_->persistent_ordinals =
            std::move(persistent_ordinals);
        receipt->impl_->persistent_entities.resize(
            persistent_count, entt::null);

        if (persistent_count != 0u)
        {
            auto claim = lux::ecs::claimPersistentEntityIds(
                impl_->persistent_entities, persistent_ids);
            if (!claim)
            {
                return lux::cxx::unexpected(materializerFailure(
                    EEntityBatchError::REGISTRY_DRIFT,
                    section,
                    generation,
                    claim.error() ==
                            lux::ecs::EPersistentEntityIdError::INVALID_ID
                        ? "persistent entity id is invalid"
                        : "persistent entity id is active or already claimed"));
            }
            receipt->impl_->persistent_claim = std::move(*claim);
        }
        auto [iterator, inserted] = impl_->slots.try_emplace(section.value());
        if (!inserted)
            publicationInvariantFailed();
        auto& slot = iterator->second;
        receipt->impl_->entities = std::move(prepared.publication_entities);
        receipt->impl_->blob_leases = std::move(prepared.blob_leases);
        slot.state = Impl::ESlotState::ARMED;
        slot.receipt = std::move(receipt);
        slot.publication_reservation =
            std::move(*publication_reservation);
        slot.reserved_entities = image.entities.size();
        slot.reserved_parents = image.parents.size();
        slot.reserved_persistent_entities = persistent_count;
        slot.reserved_components.reserve(prepared.schemas.size());
        for (std::size_t schema = 0u;
             schema < prepared.schemas.size();
             ++schema)
        {
            const auto count = prepared.schema_counts[schema];
            if (count == 0u)
                continue;
            const auto& schema_name = prepared.schemas[schema].schema_id.name;
            auto& pending = impl_->armed_components[schema_name];
            if (!lux::ecs::checkedAdditionalCapacity(
                    pending, count, pending))
            {
                publicationInvariantFailed();
            }
            slot.reserved_components.emplace_back(schema_name, count);
        }
        impl_->armed_entities = reserved_entities;
        impl_->armed_parents = reserved_parents;
        if (!lux::ecs::checkedAdditionalCapacity(
                impl_->armed_persistent_entities,
                persistent_count,
                impl_->armed_persistent_entities))
        {
            publicationInvariantFailed();
        }
        impl_->registry = &live;
        prepared.armed_registry = &live;
        prepared.state = EPreparedEntityBatchState::ARMED;
        return {};
    }

    const SectionCommitReceipt& EntityBatchMaterializer::publishAtBarrier(
        PreparedEntityBatch& batch,
        lux::meta::EntityRegistry& live) noexcept
    {
        if (!batch.impl_ ||
            batch.impl_->state != EPreparedEntityBatchState::ARMED ||
            batch.impl_->armed_registry != &live ||
            impl_->registry != &live)
        {
            publicationInvariantFailed();
        }
        auto& prepared = *batch.impl_;
        const auto& image = prepared.decoded.image_;
        const auto found = impl_->slots.find(image.section.value());
        if (found == impl_->slots.end() ||
            found->second.state != Impl::ESlotState::ARMED ||
            !found->second.receipt ||
            found->second.receipt->generation() !=
                prepared.decoded.generation())
        {
            publicationInvariantFailed();
        }
        auto& slot = found->second;
        auto& receipt = *slot.receipt;

        {
            auto publication_scope =
                slot.publication_reservation.enter();
            if (!publication_scope)
                publicationInvariantFailed();

            for (auto& entity : receipt.impl_->entities)
                entity = live.create();

            for (const auto& relocation : prepared.relocations)
            {
                void* component = prepared.schemas[relocation.schema]
                    .operations.get(
                    *prepared.staging,
                    prepared.staging_entities[relocation.source]);
                if (!component)
                    publicationInvariantFailed();
                auto* field = reinterpret_cast<entt::entity*>(
                    static_cast<std::byte*>(component) +
                    relocation.field_offset);
                *field = receipt.impl_->entities[relocation.target];
            }

            // Identity is opt-in, but when present it is part of the entity's
            // observable fact set. Commit the arm-time claim before cooked
            // component observers run so they never see a partially
            // identified entity and no second transaction can win between
            // arm and barrier.
            for (std::size_t index = 0u;
                 index < receipt.impl_->persistent_ordinals.size();
                 ++index)
            {
                receipt.impl_->persistent_entities[index] =
                    receipt.impl_->entities[
                        receipt.impl_->persistent_ordinals[index]];
            }
            if (!receipt.impl_->persistent_entities.empty())
            {
                lux::ecs::commitPersistentEntityIds(
                    impl_->persistent_entities,
                    receipt.impl_->persistent_claim,
                    receipt.impl_->persistent_entities);
            }

            for (std::size_t archetype_index = 0u;
                 archetype_index < image.archetypes.size();
                 ++archetype_index)
            {
                const auto& archetype = image.archetypes[archetype_index];
                for (const auto schema_index : archetype.schemas)
                {
                    const auto& schema = prepared.schemas[schema_index];
                    for (std::size_t value = 0u;
                         value <
                            prepared.archetype_counts[archetype_index];
                         ++value)
                    {
                        const auto ordinal =
                            prepared.archetype_first[archetype_index] + value;
                        if (!schema.operations.transfer(
                                *prepared.staging,
                                prepared.staging_entities[ordinal],
                                live,
                                receipt.impl_->entities[ordinal]))
                        {
                            publicationInvariantFailed();
                        }
                    }
                }
            }

            for (const auto& relation : image.parents)
            {
                if (!lux::ecs::setParent(
                        live,
                        receipt.impl_->entities[relation.child],
                        receipt.impl_->entities[relation.parent]))
                {
                    publicationInvariantFailed();
                }
            }
        }
        prepared.staging.reset();
        prepared.state = EPreparedEntityBatchState::PUBLISHED;
        impl_->releaseArmedReservation(slot);
        slot.state = Impl::ESlotState::ACTIVE;
        ++impl_->published_batches;
        return receipt;
    }

    lux::cxx::expected<void, EntityBatchFailure>
    EntityBatchMaterializer::cancelArmed(
        PreparedEntityBatch& batch,
        lux::meta::EntityRegistry& live) noexcept
    {
        if (!batch.impl_ ||
            batch.impl_->state != EPreparedEntityBatchState::ARMED)
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::NOT_READY,
                batch.section(),
                batch.generation(),
                "EntitySection has no armed publication to cancel"));
        }
        auto& prepared = *batch.impl_;
        if (prepared.armed_registry != &live || impl_->registry != &live)
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::REGISTRY_MISMATCH,
                prepared.decoded.section(),
                prepared.decoded.generation(),
                "armed EntitySection belongs to another registry"));
        }
        const auto found = impl_->slots.find(
            prepared.decoded.section().value());
        if (found == impl_->slots.end() ||
            found->second.state != Impl::ESlotState::ARMED ||
            !found->second.receipt ||
            found->second.receipt->generation() !=
                prepared.decoded.generation())
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::STALE_GENERATION,
                prepared.decoded.section(),
                prepared.decoded.generation(),
                "armed EntitySection slot or generation no longer matches"));
        }

        impl_->releaseArmedReservation(found->second);
        impl_->slots.erase(found);
        prepared.staging.reset();
        prepared.armed_registry = nullptr;
        prepared.state = EPreparedEntityBatchState::CANCELLED;
        return {};
    }

    lux::cxx::expected<void, EntityBatchFailure>
    EntityBatchMaterializer::deactivate(
        const lux::entity_scene::EntitySectionId& section,
        std::uint64_t generation,
        lux::meta::EntityRegistry& live) noexcept
    {
        if (impl_->registry && impl_->registry != &live)
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::REGISTRY_MISMATCH,
                section,
                generation,
                "EntityBatchMaterializer is bound to another registry"));
        }
        const auto found = impl_->slots.find(section.value());
        if (found == impl_->slots.end() ||
            found->second.state != Impl::ESlotState::ACTIVE ||
            !found->second.receipt)
        {
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::SECTION_NOT_ACTIVE,
                section,
                generation,
                "EntitySection is not active"));
        }
        auto& slot = found->second;
        if (slot.receipt->generation() != generation)
        {
            ++impl_->stale_rejections;
            return lux::cxx::unexpected(materializerFailure(
                EEntityBatchError::STALE_GENERATION,
                section,
                generation,
                "deactivation generation does not match active Section"));
        }
        for (const auto entity : slot.receipt->impl_->entities)
        {
            // Receipt handles include the EnTT version. Gameplay may destroy
            // a streamed entity before its Section retires; in that case the
            // old handle must be skipped so an entity which reused the same
            // index is never destroyed by this Section.
            if (!live.valid(entity))
            {
                ++impl_->already_destroyed_entities;
                continue;
            }
            live.destroy(entity);
        }
        impl_->slots.erase(found);
        ++impl_->deactivated_batches;
        return {};
    }

    const SectionCommitReceipt* EntityBatchMaterializer::find(
        const lux::entity_scene::EntitySectionId& section) const noexcept
    {
        const auto found = impl_->slots.find(section.value());
        return found != impl_->slots.end() &&
                found->second.state == Impl::ESlotState::ACTIVE
            ? found->second.receipt.get()
            : nullptr;
    }

    EntityBatchMaterializerSnapshot EntityBatchMaterializer::snapshot()
        const noexcept
    {
        EntityBatchMaterializerSnapshot result;
        result.published_batches = impl_->published_batches;
        result.deactivated_batches = impl_->deactivated_batches;
        result.already_destroyed_entities =
            impl_->already_destroyed_entities;
        result.stale_rejections = impl_->stale_rejections;
        result.armed_entities = impl_->armed_entities;
        result.armed_parent_edges = impl_->armed_parents;
        result.armed_persistent_entities =
            impl_->armed_persistent_entities;
        for (const auto& [schema, count] : impl_->armed_components)
        {
            static_cast<void>(schema);
            result.armed_components += count;
        }
        for (const auto& [key, slot] : impl_->slots)
        {
            static_cast<void>(key);
            if (slot.state == Impl::ESlotState::ACTIVE && slot.receipt)
            {
                ++result.active_sections;
                result.active_entities += slot.receipt->impl_->entities.size();
            }
            else if (slot.state == Impl::ESlotState::ARMED)
            {
                ++result.armed_sections;
            }
        }
        return result;
    }
}
