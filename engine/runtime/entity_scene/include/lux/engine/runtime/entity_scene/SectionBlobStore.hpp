#pragma once
/**
 * @file SectionBlobStore.hpp
 * @brief Runtime ownership of content-addressed EntitySection attachments.
 */

#include <lux/engine/ecs/entity_scene/ContentBlobStorage.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>

#include <cstdint>
#include <memory>

namespace lux::runtime::entity_scene
{
    namespace detail
    {
        struct SectionBlobEntry;
        struct SectionBlobStoreControl;
    }

    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC SectionBlobStore final
        : public lux::ecs::entity_scene::IContentBlobStorage
    {
    public:
        SectionBlobStore();
        ~SectionBlobStore() override;

        SectionBlobStore(const SectionBlobStore&) = delete;
        SectionBlobStore& operator=(const SectionBlobStore&) = delete;
        SectionBlobStore(SectionBlobStore&&) noexcept;
        SectionBlobStore& operator=(SectionBlobStore&&) noexcept;

        [[nodiscard]] lux::cxx::expected<
            lux::ecs::entity_scene::ContentBlobLease,
            lux::ecs::entity_scene::EntityBatchFailure>
        acquire(
            lux::ecs::scene_format::EntitySectionAttachment attachment,
            const lux::ecs::scene_format::EntitySectionId& section,
            std::uint64_t generation) noexcept override;

        [[nodiscard]] lux::ecs::entity_scene::ContentBlobClient client()
            const noexcept override;
        [[nodiscard]] lux::ecs::entity_scene::ContentBlobStorageSnapshot
        snapshot() const noexcept override;
        void pruneExpired() noexcept override;

    private:
        std::shared_ptr<detail::SectionBlobStoreControl> control_;
    };
} // namespace lux::runtime::entity_scene
