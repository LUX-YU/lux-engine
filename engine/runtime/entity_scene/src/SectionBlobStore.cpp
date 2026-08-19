#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>

#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>

#include "EntityBatchInternal.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace lux::runtime::entity_scene::detail
{
    namespace
    {
        [[nodiscard]] std::uint64_t nextStoreGeneration() noexcept
        {
            static std::atomic<std::uint64_t> next{1u};
            auto generation = next.fetch_add(1u, std::memory_order_relaxed);
            while (generation == 0u)
                generation = next.fetch_add(1u, std::memory_order_relaxed);
            return generation;
        }
    }

    struct SectionBlobStoreControl final
    {
        SectionBlobStoreControl() noexcept
            : generation(nextStoreGeneration())
        {}

        mutable std::mutex mutex;
        std::map<
            lux::ecs::scene_format::ContentBlobId,
            std::weak_ptr<const SectionBlobEntry>> entries;
        const std::uint64_t generation;
        bool owner_alive{true};
        std::size_t current_bytes{0u};
        std::size_t high_water_bytes{0u};
        std::size_t allocation_count{0u};
        std::size_t high_water_allocation_count{0u};
    };

    struct SectionBlobEntry final
    {
        ~SectionBlobEntry()
        {
            if (!control)
                return;
            std::lock_guard lock{control->mutex};
            const auto found = control->entries.find(reference.id);
            if (found != control->entries.end() && found->second.expired())
                control->entries.erase(found);
            if (accounted)
            {
                control->current_bytes -= bytes.size();
                --control->allocation_count;
            }
        }

        std::shared_ptr<SectionBlobStoreControl> control;
        lux::ecs::scene_format::ContentBlobRef reference;
        lux::cxx::SharedBytes<> bytes;
        bool accounted{false};
    };
}

namespace lux::runtime::entity_scene
{
    ContentBlobLease::operator bool() const noexcept
    {
        return static_cast<bool>(entry_);
    }

    const lux::ecs::scene_format::ContentBlobRef& ContentBlobLease::reference()
        const noexcept
    {
        static const lux::ecs::scene_format::ContentBlobRef empty;
        return entry_ ? entry_->reference : empty;
    }

    lux::cxx::SharedBytes<> ContentBlobLease::bytes() const noexcept
    {
        return entry_ ? entry_->bytes : lux::cxx::SharedBytes<>{};
    }

    lux::cxx::expected<ContentBlobLease, EContentBlobLookupError>
    ContentBlobClient::resolve(
        const lux::ecs::scene_format::ContentBlobRef& reference) const noexcept
    {
        if (!reference.valid())
        {
            return lux::cxx::unexpected(
                EContentBlobLookupError::INVALID_REFERENCE);
        }
        auto control = control_.lock();
        if (!control)
        {
            return lux::cxx::unexpected(
                EContentBlobLookupError::OWNER_EXPIRED);
        }

        std::shared_ptr<const detail::SectionBlobEntry> entry;
        {
            std::lock_guard lock{control->mutex};
            if (!control->owner_alive || control->generation != generation_)
            {
                return lux::cxx::unexpected(
                    EContentBlobLookupError::OWNER_EXPIRED);
            }
            const auto found = control->entries.find(reference.id);
            if (found == control->entries.end() ||
                !(entry = found->second.lock()))
            {
                return lux::cxx::unexpected(
                    EContentBlobLookupError::NOT_FOUND);
            }
        }
        if (entry->reference != reference)
        {
            return lux::cxx::unexpected(
                EContentBlobLookupError::REFERENCE_MISMATCH);
        }
        return ContentBlobLease{std::move(entry)};
    }

    ContentBlobClient::operator bool() const noexcept
    {
        const auto control = control_.lock();
        if (!control)
            return false;
        std::lock_guard lock{control->mutex};
        return control->owner_alive && control->generation == generation_;
    }

    SectionBlobStore::SectionBlobStore()
        : control_(std::make_shared<detail::SectionBlobStoreControl>())
    {}

    SectionBlobStore::~SectionBlobStore()
    {
        if (!control_)
            return;
        std::lock_guard lock{control_->mutex};
        control_->owner_alive = false;
    }

    SectionBlobStore::SectionBlobStore(SectionBlobStore&&) noexcept = default;
    SectionBlobStore& SectionBlobStore::operator=(
        SectionBlobStore&& other) noexcept
    {
        if (this == &other)
            return *this;
        if (control_)
        {
            std::lock_guard lock{control_->mutex};
            control_->owner_alive = false;
        }
        control_ = std::move(other.control_);
        return *this;
    }

    lux::cxx::expected<ContentBlobLease, EntityBatchFailure>
    SectionBlobStore::acquire(
        lux::ecs::scene_format::EntitySectionAttachment attachment,
        const lux::ecs::scene_format::EntitySectionId& section,
        std::uint64_t generation) noexcept
    {
        const auto control = control_;
        if (!control || !attachment.reference.valid())
        {
            return lux::cxx::unexpected(detail::makeFailure(
                EEntityBatchError::ATTACHMENT_FAILURE,
                section,
                generation,
                "invalid content blob reference"));
        }
        const auto computed = lux::ecs::scene_format::makeContentBlobId(
            attachment.reference.type,
            attachment.reference.schema_version,
            attachment.payload);
        if (computed != attachment.reference.id)
        {
            return lux::cxx::unexpected(detail::makeFailure(
                EEntityBatchError::ATTACHMENT_FAILURE,
                section,
                generation,
                "content blob digest mismatch"));
        }

        std::shared_ptr<const detail::SectionBlobEntry> existing;
        {
            std::lock_guard lock{control->mutex};
            if (!control->owner_alive)
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::ATTACHMENT_FAILURE,
                    section,
                    generation,
                    "SectionBlobStore owner has expired"));
            }
            const auto found = control->entries.find(
                attachment.reference.id);
            if (found != control->entries.end())
            {
                existing = found->second.lock();
                if (!existing)
                    control->entries.erase(found);
            }
        }
        if (existing)
        {
            const auto existing_bytes = existing->bytes.view();
            if (existing->reference != attachment.reference ||
                existing_bytes.size() != attachment.payload.size() ||
                !std::equal(
                    existing_bytes.begin(),
                    existing_bytes.end(),
                    attachment.payload.begin()))
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::ATTACHMENT_FAILURE,
                    section,
                    generation,
                    "content-address collision in SectionBlobStore"));
            }
            return ContentBlobLease{std::move(existing)};
        }

        auto storage = std::make_shared<std::vector<std::byte>>(
            std::move(attachment.payload));
        const auto view = std::span<const std::byte>{
            storage->data(), storage->size()};
        auto shared = lux::cxx::SharedBytes<>::fromOwner(
            std::shared_ptr<const void>{storage, view.data()}, view);
        if (shared.empty())
        {
            return lux::cxx::unexpected(detail::makeFailure(
                EEntityBatchError::ATTACHMENT_FAILURE,
                section,
                generation,
                "failed to establish owning content blob bytes"));
        }

        auto entry = std::make_shared<detail::SectionBlobEntry>();
        entry->control = control;
        entry->reference = std::move(attachment.reference);
        entry->bytes = std::move(shared);

        // Recheck while publishing so concurrent resolvers/acquirers cannot
        // create two live entries for one content address.
        {
            std::lock_guard lock{control->mutex};
            if (!control->owner_alive)
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::ATTACHMENT_FAILURE,
                    section,
                    generation,
                    "SectionBlobStore owner has expired"));
            }
            const auto found = control->entries.find(entry->reference.id);
            if (found != control->entries.end())
                existing = found->second.lock();
            if (!existing)
            {
                if (found != control->entries.end())
                    control->entries.erase(found);
                const auto [inserted_at, inserted] =
                    control->entries.emplace(entry->reference.id, entry);
                static_cast<void>(inserted_at);
                if (!inserted)
                {
                    return lux::cxx::unexpected(detail::makeFailure(
                        EEntityBatchError::INTERNAL_INVARIANT,
                        section,
                        generation,
                        "content blob lookup publication failed"));
                }
                entry->accounted = true;
                control->current_bytes += entry->bytes.size();
                ++control->allocation_count;
                control->high_water_bytes = std::max(
                    control->high_water_bytes, control->current_bytes);
                control->high_water_allocation_count = std::max(
                    control->high_water_allocation_count,
                    control->allocation_count);
            }
        }
        if (existing)
        {
            const auto existing_bytes = existing->bytes.view();
            if (existing->reference != entry->reference ||
                existing_bytes.size() != entry->bytes.size() ||
                !std::equal(
                    existing_bytes.begin(),
                    existing_bytes.end(),
                    entry->bytes.view().begin()))
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::ATTACHMENT_FAILURE,
                    section,
                    generation,
                    "content-address collision in SectionBlobStore"));
            }
            return ContentBlobLease{std::move(existing)};
        }
        return ContentBlobLease{std::move(entry)};
    }
    ContentBlobClient SectionBlobStore::client() const noexcept
    {
        const auto control = control_;
        if (!control)
            return {};
        std::lock_guard lock{control->mutex};
        return control->owner_alive
            ? ContentBlobClient{control, control->generation}
            : ContentBlobClient{};
    }

    SectionBlobStoreSnapshot SectionBlobStore::snapshot() const noexcept
    {
        const auto control = control_;
        if (!control)
            return {};
        std::lock_guard lock{control->mutex};
        return SectionBlobStoreSnapshot{
            control->current_bytes,
            control->high_water_bytes,
            control->allocation_count,
            control->high_water_allocation_count,
            control->entries.size()};
    }

    void SectionBlobStore::pruneExpired() noexcept
    {
        const auto control = control_;
        if (!control)
            return;
        std::lock_guard lock{control->mutex};
        for (auto iterator = control->entries.begin();
             iterator != control->entries.end();)
        {
            if (iterator->second.expired())
                iterator = control->entries.erase(iterator);
            else
                ++iterator;
        }
    }
}
