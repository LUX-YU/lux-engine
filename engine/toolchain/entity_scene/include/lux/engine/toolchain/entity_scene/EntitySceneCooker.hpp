#pragma once
/**
 * @file EntitySceneCooker.hpp
 * @brief Generic, domain-neutral SceneDescription/LXES cook construction.
 *
 * EntitySection images belong to ecs::scene_format. Scene selection, required
 * extensions and Section source recipes belong to Engine SceneDescription. The
 * cooker deliberately exposes only those canonical owners.
 */

#include <lux/engine/toolchain/entity_scene/EntitySceneCookError.hpp>

#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/engine/scene/SceneDescription.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <vector>

namespace lux::toolchain
{
    struct EntitySectionCookInput final
    {
        lux::ecs::scene_format::EntitySectionImage image;
        lux::ecs::scene_format::SectionSource source;
        std::vector<lux::ecs::scene_format::EntitySectionId> dependencies;
        std::vector<lux::ecs::scene_format::DemandChannelId> demand_channels;
    };

    struct SceneDescriptionCookInput final
    {
        lux::asset::asset_id_t id;
        std::vector<std::byte> spatial3d_catalog;
        std::vector<lux::ecs::scene_format::EntitySectionId> startup_sections;
        std::vector<EntitySectionCookInput> sections;
        std::vector<lux::scene::RequiredExtension> required_extensions;
        /// Extra derived schemas absent from Section images. Schemas present
        /// in an image are added automatically.
        std::vector<lux::ecs::scene_format::RequiredComponentSchema> required_components;
        /// Build-configuration roots. These are consumed by Cook and become
        /// derived LXSC requirements; no runtime usage-manifest object exists.
        std::vector<std::string> project_required_render_features;
        std::vector<std::string> project_optional_render_features;
    };

    struct CookedEntitySection final
    {
        lux::ecs::scene_format::SectionRecord record;
        lux::ecs::scene_format::EntitySectionImage image;
        /// Canonical decoded LXES bytes. Stored sources publish these bytes;
        /// generated sources may retain them as deterministic expectation data.
        std::vector<std::byte> encoded_image;
    };

    /// Domain-neutral base result. Domain adapters may derive solely to append
    /// sidecar outputs (for example generated Mesh assets); the SceneDescription
    /// and LXES fields remain unaware of those domains.
    struct CookedSceneDescriptionBundle
    {
        lux::scene::SceneDescription package;
        std::vector<std::byte> encoded_package;
        /// Same UUID order as package.sections.
        std::vector<CookedEntitySection> sections;
    };

    /// Validates and encodes every canonical LXES image, derives Section record
    /// digests/counts/component requirements, canonicalizes package
    /// collections, and
    /// finally encodes LXSC v2 through the Engine-owned SceneDescription codec.
    /// Compression is deliberately outside this generic target; all records
    /// emitted here use SectionCompression::NONE and exact encoded sizes.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<CookedSceneDescriptionBundle, EntitySceneCookFailure>
    cookSceneDescription(SceneDescriptionCookInput input) noexcept;
} // namespace lux::toolchain
