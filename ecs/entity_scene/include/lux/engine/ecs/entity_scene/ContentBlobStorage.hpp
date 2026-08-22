#pragma once
/**
 * @file ContentBlobStorage.hpp
 * @brief ECS-facing ownership seam for cooked EntitySection attachments.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/ecs/entity_scene/EntityBatchTypes.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::ecs::entity_scene
{
    enum class EContentBlobLookupError : std::uint8_t
    {
        INVALID_REFERENCE,
        OWNER_EXPIRED,
        NOT_FOUND,
        REFERENCE_MISMATCH
    };

    struct ContentBlobStorageSnapshot final
    {
        std::size_t current_bytes{0u};
        std::size_t high_water_bytes{0u};
        std::size_t allocation_count{0u};
        std::size_t high_water_allocation_count{0u};
        std::size_t lookup_entries{0u};
    };

    class ContentBlobLease final
    {
    public:
        ContentBlobLease() noexcept = default;
        ContentBlobLease(ContentBlobLease&&) noexcept = default;
        ContentBlobLease& operator=(ContentBlobLease&&) noexcept = default;
        ContentBlobLease(const ContentBlobLease&) = delete;
        ContentBlobLease& operator=(const ContentBlobLease&) = delete;

        ContentBlobLease(
            lux::ecs::scene_format::ContentBlobRef reference,
            lux::cxx::SharedBytes<> bytes,
            std::shared_ptr<const void> lifetime) noexcept
            : reference_(std::move(reference)),
              bytes_(std::move(bytes)),
              lifetime_(std::move(lifetime))
        {}

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(lifetime_);
        }

        [[nodiscard]] const lux::ecs::scene_format::ContentBlobRef&
        reference() const noexcept
        {
            return reference_;
        }

        [[nodiscard]] lux::cxx::SharedBytes<> bytes() const noexcept
        {
            return bytes_;
        }

    private:
        lux::ecs::scene_format::ContentBlobRef reference_;
        lux::cxx::SharedBytes<> bytes_;
        std::shared_ptr<const void> lifetime_;
    };

    class ContentBlobClient final
    {
    public:
        using ResolveFn = lux::cxx::expected<
            ContentBlobLease,
            EContentBlobLookupError> (*)(
                const void*,
                const lux::ecs::scene_format::ContentBlobRef&) noexcept;
        using IsAliveFn = bool (*)(const void*) noexcept;

        ContentBlobClient() noexcept = default;

        ContentBlobClient(
            std::shared_ptr<const void> state,
            ResolveFn resolve,
            IsAliveFn is_alive) noexcept
            : state_(std::move(state)),
              resolve_(resolve),
              is_alive_(is_alive)
        {}

        [[nodiscard]] lux::cxx::expected<
            ContentBlobLease,
            EContentBlobLookupError>
        resolve(
            const lux::ecs::scene_format::ContentBlobRef& reference) const
            noexcept
        {
            if (!state_ || !resolve_)
                return lux::cxx::unexpected(
                    EContentBlobLookupError::OWNER_EXPIRED);
            return resolve_(state_.get(), reference);
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return state_ && is_alive_ && is_alive_(state_.get());
        }

    private:
        std::shared_ptr<const void> state_;
        ResolveFn resolve_{nullptr};
        IsAliveFn is_alive_{nullptr};
    };

    class IContentBlobStorage
    {
    public:
        virtual ~IContentBlobStorage() = default;

        [[nodiscard]] virtual lux::cxx::expected<
            ContentBlobLease,
            EntityBatchFailure>
        acquire(
            lux::ecs::scene_format::EntitySectionAttachment attachment,
            const lux::ecs::scene_format::EntitySectionId& section,
            std::uint64_t generation) noexcept = 0;

        [[nodiscard]] virtual ContentBlobClient client() const noexcept = 0;
        [[nodiscard]] virtual ContentBlobStorageSnapshot snapshot() const
            noexcept = 0;
        virtual void pruneExpired() noexcept = 0;
    };
} // namespace lux::ecs::entity_scene
