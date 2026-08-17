#pragma once
/**
 * @file PersistentEntityIndex.hpp
 * @brief Sparse, registry-bound authority for opt-in persistent identity.
 *
 * PersistentEntityIdComponent is deliberately not a freely writable value.
 * Every public mutation goes through the helpers in this header so the ECS
 * fact and its sparse lookup index are committed by one operation. Section
 * publication reserves IDs while it is armed; another transaction cannot
 * claim the same ID before the command barrier publishes the first one.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/components/PersistentEntityIdComponent.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <entt/entt.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace lux::ecs
{
    enum class EPersistentEntityIdError : std::uint8_t
    {
        INVALID_ENTITY,
        INVALID_ID,
        DUPLICATE_ID
    };

    template <typename T>
    using PersistentEntityIdExp = lux::cxx::expected<T, EPersistentEntityIdError>;

    using PersistentEntityIdResult = PersistentEntityIdExp<void>;

    class PersistentEntityIndex;

    /// Move-only reservation owned by one unpublished ECS transaction.
    /// A live claim must not outlive the registry which issued it.
    class PersistentEntityIdClaim final
    {
    public:
        PersistentEntityIdClaim() noexcept = default;
        ~PersistentEntityIdClaim() noexcept;
        PersistentEntityIdClaim(const PersistentEntityIdClaim&) = delete;
        PersistentEntityIdClaim& operator=(
            const PersistentEntityIdClaim&) = delete;
        PersistentEntityIdClaim(PersistentEntityIdClaim&& other) noexcept;
        PersistentEntityIdClaim& operator=(
            PersistentEntityIdClaim&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return owner_ != nullptr;
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return ids_.size();
        }

        /// Releases an unpublished reservation. A committed claim is already
        /// empty, so reset() is idempotent.
        void reset() noexcept;

    private:
        friend class PersistentEntityIndex;

        PersistentEntityIdClaim(
            PersistentEntityIndex& owner,
            std::weak_ptr<std::uint8_t> lifetime,
            std::uint64_t token,
            std::vector<lux::entity_scene::PersistentEntityId> ids) noexcept
            : owner_(&owner),
              lifetime_(std::move(lifetime)),
              token_(token),
              ids_(std::move(ids))
        {}

        PersistentEntityIndex* owner_{nullptr};
        std::weak_ptr<std::uint8_t> lifetime_;
        std::uint64_t token_{0u};
        std::vector<lux::entity_scene::PersistentEntityId> ids_;
    };

    using PersistentEntityIdClaimResult = PersistentEntityIdExp<PersistentEntityIdClaim>;

    namespace detail
    {
        [[noreturn]] inline void persistentEntityInvariantFailed() noexcept
        {
            std::abort();
        }
    }

    /// Registry-bound authority with an explicit owner lifetime. It indexes
    /// only entities which opt in through PersistentEntityIdComponent and
    /// owns the EnTT signal connections which keep that sparse index current.
    /// Composition code must keep this object alive while its clients use the
    /// bound registry; there is deliberately no implicit service lookup.
    class PersistentEntityIndex final
    {
    public:
        explicit PersistentEntityIndex(
            lux::meta::EntityRegistryBase& registry)
            : registry_(&registry),
              constructed_(
                  registry.on_construct<PersistentEntityIdComponent>()
                      .connect<&PersistentEntityIndex::observeConstruct>(
                          *this)),
              updated_(
                  registry.on_update<PersistentEntityIdComponent>()
                      .connect<&PersistentEntityIndex::observeUpdate>(*this)),
              destroyed_(
                  registry.on_destroy<PersistentEntityIdComponent>()
                      .connect<&PersistentEntityIndex::observeDestroy>(*this))
        {
            foldExisting();
        }

        ~PersistentEntityIndex() noexcept
        {
            lifetime_.reset();
        }

        PersistentEntityIndex(const PersistentEntityIndex&) = delete;
        PersistentEntityIndex& operator=(const PersistentEntityIndex&) =
            delete;

        [[nodiscard]] bool boundTo(
            const lux::meta::EntityRegistryBase& registry) const noexcept
        {
            return registry_ == &registry;
        }

        [[nodiscard]] entt::entity find(
            const lux::entity_scene::PersistentEntityId& id) const noexcept
        {
            const auto found = findIdEntry(id.value());
            return found == by_id_.end() ? entt::null : found->second;
        }

        [[nodiscard]] bool contains(entt::entity entity) const noexcept
        {
            return findEntityEntry(entity) != by_entity_.end();
        }

        [[nodiscard]] bool claimed(
            const lux::entity_scene::PersistentEntityId& id) const noexcept
        {
            return findPendingEntry(id.value()) != pending_.end();
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return by_id_.size();
        }

        [[nodiscard]] std::size_t pendingCount() const noexcept
        {
            return pending_.size();
        }

        /// Preallocates both active directions. Signal callbacks never grow
        /// these arrays: failure to reserve before an authorised construct is
        /// an engine invariant rather than a silently divergent index.
        void reserve(std::size_t additional)
        {
            // Pending claims can all become active in the same command
            // barrier.  Account for them here so separately armed Sections
            // do not each reserve from the same active-size baseline and
            // exhaust the signal-side arrays during the second commit.
            const auto required = by_id_.size() + pending_.size() +
                                  additional;
            by_id_.reserve(required);
            by_entity_.reserve(required);
        }

        [[nodiscard]] PersistentEntityIdClaimResult claim(
            std::span<const lux::entity_scene::PersistentEntityId> ids)
        {
            std::vector<lux::entity_scene::PersistentEntityId> owned;
            owned.reserve(ids.size());
            for (const auto& id : ids)
            {
                if (id.empty())
                {
                    return lux::cxx::unexpected(
                        EPersistentEntityIdError::INVALID_ID);
                }
                if (find(id) != entt::null || claimed(id) ||
                    std::find(owned.begin(), owned.end(), id) != owned.end())
                {
                    return lux::cxx::unexpected(
                        EPersistentEntityIdError::DUPLICATE_ID);
                }
                owned.push_back(id);
            }

            if (owned.empty())
                return PersistentEntityIdClaim{};

            reserve(owned.size());
            pending_.reserve(pending_.size() + owned.size());
            std::uint64_t token = 0u;
            do
            {
                token = allocateClaimToken();
            }
            while (std::any_of(
                pending_.begin(),
                pending_.end(),
                [token](const auto& entry)
                {
                    return entry.second == token;
                }));
            for (const auto& id : owned)
                pending_.emplace_back(id.value(), token);
            return PersistentEntityIdClaim{
                *this, lifetime_, token, std::move(owned)};
        }

        /// Commits a complete pending claim. All checks precede the first ECS
        /// mutation; after this point any mismatch is an invariant violation.
        void commit(
            PersistentEntityIdClaim& claim,
            std::span<const entt::entity> entities) noexcept
        {
            if (claim.owner_ != this || claim.lifetime_.expired() ||
                claim.token_ == 0u || entities.size() != claim.ids_.size())
            {
                detail::persistentEntityInvariantFailed();
            }

            for (std::size_t index = 0u; index < entities.size(); ++index)
            {
                if (!registry_->valid(entities[index]) ||
                    registry_->all_of<PersistentEntityIdComponent>(
                        entities[index]) ||
                    find(claim.ids_[index]) != entt::null)
                {
                    detail::persistentEntityInvariantFailed();
                }
                const auto pending = findPendingEntry(
                    claim.ids_[index].value());
                if (pending == pending_.end() ||
                    pending->second != claim.token_)
                {
                    detail::persistentEntityInvariantFailed();
                }
            }

            const auto token = claim.token_;
            auto ids = std::move(claim.ids_);
            // Detach before the first EnTT signal: user observers run inside
            // emplace and may destroy the publishing owner/receipt. No active
            // signal callback can then re-enter a claim destructor that still
            // points at this index.
            claim.owner_ = nullptr;
            claim.lifetime_.reset();
            claim.token_ = 0u;

            for (std::size_t index = 0u; index < entities.size(); ++index)
            {
                beginMutation(
                    EMutation::CONSTRUCT,
                    entities[index],
                    ids[index].value(),
                    token);
                registry_->emplace<PersistentEntityIdComponent>(
                    entities[index],
                    PersistentEntityIdComponent{ids[index]});
                finishMutation();
            }

            erasePendingToken(token);
        }

        void cancel(PersistentEntityIdClaim& claim) noexcept
        {
            if (!claim.owner_)
                return;
            if (claim.owner_ != this || claim.lifetime_.expired() ||
                claim.token_ == 0u)
            {
                detail::persistentEntityInvariantFailed();
            }
            erasePendingToken(claim.token_);
            detachClaim(claim);
        }

        [[nodiscard]] PersistentEntityIdResult set(
            entt::entity entity,
            lux::entity_scene::PersistentEntityId id)
        {
            if (!registry_->valid(entity))
            {
                return lux::cxx::unexpected(
                    EPersistentEntityIdError::INVALID_ENTITY);
            }
            if (id.empty())
            {
                return lux::cxx::unexpected(
                    EPersistentEntityIdError::INVALID_ID);
            }

            const auto duplicate = find(id);
            if ((duplicate != entt::null && duplicate != entity) ||
                claimed(id))
            {
                return lux::cxx::unexpected(
                    EPersistentEntityIdError::DUPLICATE_ID);
            }

            const auto* current =
                registry_->try_get<PersistentEntityIdComponent>(entity);
            if (current)
            {
                const auto by_entity = findEntityEntry(entity);
                if (by_entity == by_entity_.end() ||
                    find(current->id()) != entity)
                {
                    detail::persistentEntityInvariantFailed();
                }
                if (current->id() == id)
                    return {};

                beginMutation(
                    EMutation::UPDATE, entity, id.value(), 0u);
                registry_->patch<PersistentEntityIdComponent>(
                    entity,
                    [&id](PersistentEntityIdComponent& component) noexcept
                    {
                        component.id_ = std::move(id);
                    });
                finishMutation();
                return {};
            }

            if (contains(entity))
                detail::persistentEntityInvariantFailed();
            reserve(1u);
            const auto raw_id = id.value();
            beginMutation(EMutation::CONSTRUCT, entity, raw_id, 0u);
            registry_->emplace<PersistentEntityIdComponent>(
                entity, PersistentEntityIdComponent{std::move(id)});
            finishMutation();
            return {};
        }

        void clear(entt::entity entity) noexcept
        {
            if (!registry_->valid(entity))
                return;
            const bool has_component =
                registry_->all_of<PersistentEntityIdComponent>(entity);
            if (has_component != contains(entity))
                detail::persistentEntityInvariantFailed();
            if (has_component)
                static_cast<void>(
                    registry_->remove<PersistentEntityIdComponent>(entity));
        }

    private:
        void foldExisting()
        {
            by_id_.clear();
            by_entity_.clear();
            pending_.clear();
            const auto view =
                registry_->view<const PersistentEntityIdComponent>();
            const auto count =
                registry_->storage<PersistentEntityIdComponent>().size();
            reserve(count);
            for (const auto entity : view)
            {
                const auto& id =
                    registry_->get<PersistentEntityIdComponent>(entity).id();
                if (id.empty() || find(id) != entt::null)
                    detail::persistentEntityInvariantFailed();
                by_id_.emplace_back(id.value(), entity);
                by_entity_.emplace_back(entity, id.value());
            }
            monitoring_ = true;
        }

        void observeConstruct(
            lux::meta::EntityRegistryBase& registry,
            entt::entity entity)
            noexcept
        {
            if (!monitoring_)
                return;
            if (&registry != registry_ ||
                mutation_.kind != EMutation::CONSTRUCT ||
                mutation_.entity != entity || mutation_.consumed)
            {
                detail::persistentEntityInvariantFailed();
            }
            const auto* component =
                registry.try_get<PersistentEntityIdComponent>(entity);
            if (!component || component->id().empty() ||
                component->id().value() != mutation_.id ||
                find(component->id()) != entt::null || contains(entity) ||
                by_id_.size() == by_id_.capacity() ||
                by_entity_.size() == by_entity_.capacity())
            {
                detail::persistentEntityInvariantFailed();
            }
            const auto pending = findPendingEntry(component->id().value());
            if ((mutation_.claim_token == 0u && pending != pending_.end()) ||
                (mutation_.claim_token != 0u &&
                 (pending == pending_.end() ||
                  pending->second != mutation_.claim_token)))
            {
                detail::persistentEntityInvariantFailed();
            }
            by_id_.emplace_back(component->id().value(), entity);
            by_entity_.emplace_back(entity, component->id().value());
            mutation_.consumed = true;
        }

        void observeUpdate(
            lux::meta::EntityRegistryBase& registry,
            entt::entity entity)
            noexcept
        {
            if (!monitoring_)
                return;
            if (&registry != registry_ ||
                mutation_.kind != EMutation::UPDATE ||
                mutation_.entity != entity || mutation_.consumed)
            {
                detail::persistentEntityInvariantFailed();
            }
            const auto* component =
                registry.try_get<PersistentEntityIdComponent>(entity);
            const auto entity_entry = findEntityEntry(entity);
            if (!component || component->id().empty() ||
                component->id().value() != mutation_.id ||
                entity_entry == by_entity_.end() || claimed(component->id()))
            {
                detail::persistentEntityInvariantFailed();
            }
            const auto duplicate = find(component->id());
            if (duplicate != entt::null && duplicate != entity)
                detail::persistentEntityInvariantFailed();

            const auto old_id = entity_entry->second;
            const auto id_entry = std::find_if(
                by_id_.begin(),
                by_id_.end(),
                [&old_id, entity](const auto& entry)
                {
                    return entry.first == old_id && entry.second == entity;
                });
            if (id_entry == by_id_.end())
                detail::persistentEntityInvariantFailed();
            id_entry->first = component->id().value();
            entity_entry->second = component->id().value();
            mutation_.consumed = true;
        }

        void observeDestroy(
            lux::meta::EntityRegistryBase& registry,
            entt::entity entity)
            noexcept
        {
            if (!monitoring_)
                return;
            if (&registry != registry_)
                detail::persistentEntityInvariantFailed();
            eraseEntity(entity);
        }

        enum class EMutation : std::uint8_t
        {
            NONE,
            CONSTRUCT,
            UPDATE
        };

        struct Mutation final
        {
            EMutation kind{EMutation::NONE};
            entt::entity entity{entt::null};
            uuids::uuid id{};
            std::uint64_t claim_token{0u};
            bool consumed{false};
        };

        using IdEntries = std::vector<std::pair<uuids::uuid, entt::entity>>;
        using EntityEntries = std::vector<std::pair<entt::entity, uuids::uuid>>;
        using PendingEntries = std::vector<std::pair<uuids::uuid, std::uint64_t>>;

        [[nodiscard]] IdEntries::iterator findIdEntry(
            const uuids::uuid& id) noexcept
        {
            return std::find_if(
                by_id_.begin(), by_id_.end(),
                [&id](const auto& entry) { return entry.first == id; });
        }

        [[nodiscard]] IdEntries::const_iterator findIdEntry(
            const uuids::uuid& id) const noexcept
        {
            return std::find_if(
                by_id_.begin(), by_id_.end(),
                [&id](const auto& entry) { return entry.first == id; });
        }

        [[nodiscard]] EntityEntries::iterator findEntityEntry(
            entt::entity entity) noexcept
        {
            return std::find_if(
                by_entity_.begin(), by_entity_.end(),
                [entity](const auto& entry)
                {
                    return entry.first == entity;
                });
        }

        [[nodiscard]] EntityEntries::const_iterator findEntityEntry(
            entt::entity entity) const noexcept
        {
            return std::find_if(
                by_entity_.begin(), by_entity_.end(),
                [entity](const auto& entry)
                {
                    return entry.first == entity;
                });
        }

        [[nodiscard]] PendingEntries::iterator findPendingEntry(
            const uuids::uuid& id) noexcept
        {
            return std::find_if(
                pending_.begin(), pending_.end(),
                [&id](const auto& entry) { return entry.first == id; });
        }

        [[nodiscard]] PendingEntries::const_iterator findPendingEntry(
            const uuids::uuid& id) const noexcept
        {
            return std::find_if(
                pending_.begin(), pending_.end(),
                [&id](const auto& entry) { return entry.first == id; });
        }

        [[nodiscard]] std::uint64_t allocateClaimToken() noexcept
        {
            ++next_claim_token_;
            if (next_claim_token_ == 0u)
                ++next_claim_token_;
            return next_claim_token_;
        }

        void beginMutation(
            EMutation kind,
            entt::entity entity,
            uuids::uuid id,
            std::uint64_t claim_token) noexcept
        {
            if (mutation_.kind != EMutation::NONE)
                detail::persistentEntityInvariantFailed();
            mutation_ = Mutation{kind, entity, id, claim_token, false};
        }

        void finishMutation() noexcept
        {
            if (!mutation_.consumed)
                detail::persistentEntityInvariantFailed();
            mutation_ = {};
        }

        void eraseEntity(entt::entity entity) noexcept
        {
            const auto found = findEntityEntry(entity);
            if (found == by_entity_.end())
            {
                // A component can only reach on_destroy after a matching
                // authorised construct or foldExisting(). Anything else is a
                // second truth source and must stop the process.
                detail::persistentEntityInvariantFailed();
            }
            const auto current = std::find_if(
                by_id_.begin(),
                by_id_.end(),
                [&found, entity](const auto& entry)
                {
                    return entry.first == found->second &&
                        entry.second == entity;
                });
            if (current == by_id_.end())
                detail::persistentEntityInvariantFailed();
            by_id_.erase(current);
            by_entity_.erase(found);
        }

        void erasePendingToken(std::uint64_t token) noexcept
        {
            const auto old_size = pending_.size();
            std::erase_if(
                pending_,
                [token](const auto& entry) { return entry.second == token; });
            if (old_size == pending_.size())
                detail::persistentEntityInvariantFailed();
        }

        static void detachClaim(PersistentEntityIdClaim& claim) noexcept
        {
            claim.owner_ = nullptr;
            claim.lifetime_.reset();
            claim.token_ = 0u;
            claim.ids_.clear();
        }

        lux::meta::EntityRegistryBase* registry_{nullptr};
        entt::scoped_connection constructed_;
        entt::scoped_connection updated_;
        entt::scoped_connection destroyed_;
        std::shared_ptr<std::uint8_t> lifetime_{
            std::make_shared<std::uint8_t>(0u)};
        // Stable identity is intentionally sparse. Flat owning arrays permit
        // arm() to reserve all signal-side storage before publication.
        IdEntries by_id_;
        EntityEntries by_entity_;
        PendingEntries pending_;
        Mutation mutation_;
        std::uint64_t next_claim_token_{0u};
        bool monitoring_{false};
    };

    inline PersistentEntityIdClaim::~PersistentEntityIdClaim() noexcept
    {
        reset();
    }

    inline PersistentEntityIdClaim::PersistentEntityIdClaim(
        PersistentEntityIdClaim&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)),
          lifetime_(std::move(other.lifetime_)),
          token_(std::exchange(other.token_, 0u)),
          ids_(std::move(other.ids_))
    {}

    inline PersistentEntityIdClaim& PersistentEntityIdClaim::operator=(
        PersistentEntityIdClaim&& other) noexcept
    {
        if (this == &other)
            return *this;
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        lifetime_ = std::move(other.lifetime_);
        token_ = std::exchange(other.token_, 0u);
        ids_ = std::move(other.ids_);
        return *this;
    }

    inline void PersistentEntityIdClaim::reset() noexcept
    {
        if (owner_ && !lifetime_.expired())
            owner_->cancel(*this);
        else
        {
            owner_ = nullptr;
            lifetime_.reset();
            token_ = 0u;
            ids_.clear();
        }
    }

    [[nodiscard]] inline PersistentEntityIdClaimResult
    claimPersistentEntityIds(
        PersistentEntityIndex& index,
        std::span<const lux::entity_scene::PersistentEntityId> ids)
    {
        return index.claim(ids);
    }

    inline void commitPersistentEntityIds(
        PersistentEntityIndex& index,
        PersistentEntityIdClaim& claim,
        std::span<const entt::entity> entities) noexcept
    {
        index.commit(claim, entities);
    }

    [[nodiscard]] inline PersistentEntityIdResult setPersistentEntityId(
        PersistentEntityIndex& index,
        entt::entity entity,
        lux::entity_scene::PersistentEntityId id)
    {
        return index.set(entity, std::move(id));
    }

    inline void clearPersistentEntityId(
        PersistentEntityIndex& index,
        entt::entity entity) noexcept
    {
        index.clear(entity);
    }
}
