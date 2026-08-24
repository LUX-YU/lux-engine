#pragma once
/**
 * @file EntitySectionImageBuilder.hpp
 * @brief Domain-neutral canonical archetype-column LXES image builder.
 */

#include <lux/engine/toolchain/entity_scene/EntitySceneCookError.hpp>
#include <lux/engine/toolchain/entity_scene/TaggedPayloadTranscoder.hpp>

#include <lux/engine/ecs/scene_format/EntitySection.hpp>

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
        lux::ecs::scene_format::EntityOrdinal target{
            lux::ecs::scene_format::kInvalidEntityOrdinal};
    };

    struct EntityPersistentReferenceCookInput final
    {
        std::string property;
        lux::ecs::PersistentEntityId target;
    };

    struct EntityBlobReferenceCookInput final
    {
        std::string property;
        /// Index returned by EntitySectionImageBuilder::addAttachment().
        std::uint32_t attachment{0u};
    };

    struct EntityComponentCookInput final
    {
        lux::ecs::ComponentSchemaId schema;
        std::uint32_t schema_version{1u};
        lux::ecs::scene_format::EEntityComponentStorage storage{
            lux::ecs::scene_format::EEntityComponentStorage::DATA};
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
        std::optional<lux::ecs::PersistentEntityId> persistent_id;
        /// Refers to the insertion ordinal returned by addEntity(). Parents
        /// may be inserted before or after their children.
        std::optional<lux::ecs::scene_format::EntityOrdinal> parent;
        std::vector<EntityComponentCookInput> components;
    };

    struct EntityAttachmentCookInput final
    {
        lux::ecs::scene_format::ContentTypeId type;
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
            lux::ecs::scene_format::EntitySectionId section) noexcept;

        [[nodiscard]] lux::cxx::expected<
            std::uint32_t,
            EntitySceneCookFailure>
        addAttachment(EntityAttachmentCookInput attachment);

        [[nodiscard]] lux::cxx::expected<
            lux::ecs::scene_format::EntityOrdinal,
            EntitySceneCookFailure>
        addEntity(EntityCookInput entity);

        [[nodiscard]] lux::cxx::expected<
            lux::ecs::scene_format::EntitySectionImage,
            EntitySceneCookFailure>
        build() && noexcept;

    private:
        lux::ecs::scene_format::EntitySectionId section_;
        std::vector<EntityAttachmentCookInput> attachments_;
        std::vector<EntityCookInput> entities_;
    };
}
