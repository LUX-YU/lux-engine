#pragma once
/**
 * @file SectionBlobStore.hpp
 * @brief Content-addressed attachment ownership for active EntitySections.
 */

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchTypes.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace lux::runtime::entity_scene
{
    enum class EContentBlobLookupError : std::uint8_t
    {
        INVALID_REFERENCE,
        OWNER_EXPIRED,
        NOT_FOUND,
        REFERENCE_MISMATCH
    };

    struct SectionBlobStoreSnapshot final
    {
        std::size_t current_bytes{0u};
        std::size_t high_water_bytes{0u};
        std::size_t allocation_count{0u};
        std::size_t high_water_allocation_count{0u};
        std::size_t lookup_entries{0u};
    };

    namespace detail
    {
        struct SectionBlobEntry;
        struct SectionBlobStoreControl;
    }

    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC ContentBlobLease final
    {
    public:
        ContentBlobLease() noexcept = default;
        ContentBlobLease(ContentBlobLease&&) noexcept = default;
        ContentBlobLease& operator=(ContentBlobLease&&) noexcept = default;
        ContentBlobLease(const ContentBlobLease&) = delete;
        ContentBlobLease& operator=(const ContentBlobLease&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] const lux::ecs::scene_format::ContentBlobRef& reference()
            const noexcept;
        [[nodiscard]] lux::cxx::SharedBytes<> bytes() const noexcept;

    private:
        friend class ContentBlobClient;
        friend class SectionBlobStore;
        explicit ContentBlobLease(
            std::shared_ptr<const detail::SectionBlobEntry> entry) noexcept
            : entry_(std::move(entry))
        {}

        std::shared_ptr<const detail::SectionBlobEntry> entry_;
    };

    /// Read-only resolver for ContentBlobRef values published into ECS.
    ///
    /// The client never owns the store. A successful resolve returns a lease
    /// which pins immutable bytes independently of Section retirement. A
    /// client from a destroyed store cannot bind to a later store generation.
    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC ContentBlobClient final
    {
    public:
        ContentBlobClient() noexcept = default;

        [[nodiscard]] lux::cxx::expected<
            ContentBlobLease,
            EContentBlobLookupError>
        resolve(
            const lux::ecs::scene_format::ContentBlobRef& reference) const noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class SectionBlobStore;
        ContentBlobClient(
            std::weak_ptr<detail::SectionBlobStoreControl> control,
            std::uint64_t generation) noexcept
            : control_(std::move(control)), generation_(generation)
        {}

        std::weak_ptr<detail::SectionBlobStoreControl> control_;
        std::uint64_t generation_{0u};
    };

    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC SectionBlobStore final
    {
    public:
        SectionBlobStore();
        ~SectionBlobStore();

        SectionBlobStore(const SectionBlobStore&) = delete;
        SectionBlobStore& operator=(const SectionBlobStore&) = delete;
        SectionBlobStore(SectionBlobStore&&) noexcept;
        SectionBlobStore& operator=(SectionBlobStore&&) noexcept;

        [[nodiscard]] lux::cxx::expected<
            ContentBlobLease,
            EntityBatchFailure>
        acquire(
            lux::ecs::scene_format::EntitySectionAttachment attachment,
            const lux::ecs::scene_format::EntitySectionId& section,
            std::uint64_t generation) noexcept;

        [[nodiscard]] ContentBlobClient client() const noexcept;
        [[nodiscard]] SectionBlobStoreSnapshot snapshot() const noexcept;
        void pruneExpired() noexcept;

    private:
        std::shared_ptr<detail::SectionBlobStoreControl> control_;
    };
}
