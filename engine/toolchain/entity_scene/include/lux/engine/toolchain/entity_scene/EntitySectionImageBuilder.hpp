#pragma once
/**
 * @file EntitySectionImageBuilder.hpp
 * @brief Domain-neutral canonical archetype-column LXES image builder.
 */

#include <lux/engine/toolchain/entity_scene/EntitySceneCookError.hpp>
#include <lux/engine/toolchain/entity_scene/TaggedPayloadTranscoder.hpp>

#include <lux/engine/resource/entity_scene/EntitySection.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lux::toolchain
{
    struct EntityLocalReferenceCookInput final
    {
        std::string property;
        lux::entity_scene::EntityOrdinal target{
            lux::entity_scene::kInvalidEntityOrdinal};
    };

    struct EntityPersistentReferenceCookInput final
    {
        std::string property;
        lux::entity_scene::PersistentEntityId target;
    };

    struct EntityBlobReferenceCookInput final
    {
        std::string property;
        /// Index returned by EntitySectionImageBuilder::addAttachment().
        std::uint32_t attachment{0u};
    };

    struct EntityComponentCookInput final
    {
        lux::entity_scene::ComponentSchemaId schema;
        std::uint32_t schema_version{1u};
        lux::entity_scene::EEntityComponentStorage storage{
            lux::entity_scene::EEntityComponentStorage::DATA};
        /// DATA components carry one complete tagged-property object. TAG
        /// components must leave this value empty/defaulted.
        TaggedPayloadSource value;
        std::vector<EntityLocalReferenceCookInput> local_references;
        std::vector<EntityPersistentReferenceCookInput>
            persistent_references;
        std::vector<EntityBlobReferenceCookInput> blob_references;
    };

    struct EntityCookInput final
    {
        std::optional<lux::entity_scene::PersistentEntityId> persistent_id;
        /// Refers to the insertion ordinal returned by addEntity(). Parents
        /// may be inserted before or after their children.
        std::optional<lux::entity_scene::EntityOrdinal> parent;
        std::vector<EntityComponentCookInput> components;
    };

    struct EntityAttachmentCookInput final
    {
        lux::entity_scene::ContentTypeId type;
        std::uint32_t schema_version{1u};
        std::vector<std::byte> payload;
    };

    /// Collects entities in caller order, then emits the canonical LXES table
    /// order: schemas by canonical name, archetypes lexicographically, entities
    /// grouped by archetype (stable within a group), and attachments by
    /// (content type, digest). All ordinal/attachment references are remapped.
    class LUX_ENGINE_TOOLCHAIN_ENTITY_SCENE_PUBLIC
    EntitySectionImageBuilder final
    {
    public:
        explicit EntitySectionImageBuilder(
            lux::entity_scene::EntitySectionId section) noexcept;

        [[nodiscard]] lux::cxx::expected<
            std::uint32_t,
            EntitySceneCookFailure>
        addAttachment(EntityAttachmentCookInput attachment);

        [[nodiscard]] lux::cxx::expected<
            lux::entity_scene::EntityOrdinal,
            EntitySceneCookFailure>
        addEntity(EntityCookInput entity);

        [[nodiscard]] lux::cxx::expected<
            lux::entity_scene::EntitySectionImage,
            EntitySceneCookFailure>
        build() && noexcept;

    private:
        lux::entity_scene::EntitySectionId section_;
        std::vector<EntityAttachmentCookInput> attachments_;
        std::vector<EntityCookInput> entities_;
    };
}
