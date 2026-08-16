#pragma once
/**
 * @file EntitySceneCooker.hpp
 * @brief Generic, domain-neutral LXSC/LXES cook bundle construction.
 */

#include <lux/engine/toolchain/entity_scene/EntitySceneCookError.hpp>

#include <lux/engine/resource/entity_scene/EntityScene.hpp>
#include <lux/engine/resource/entity_scene/EntitySection.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <vector>

namespace lux::toolchain
{
    struct EntitySectionCookInput final
    {
        lux::entity_scene::EntitySectionImage image;
        lux::entity_scene::EntitySectionSource source;
        std::vector<lux::entity_scene::EntitySectionId> dependencies;
        std::vector<lux::entity_scene::DemandChannelId> demand_channels;
        std::vector<lux::entity_scene::RequiredExtension>
            required_extensions;
    };

    struct EntitySceneCookInput final
    {
        lux::entity_scene::EntitySceneId id;
        std::vector<lux::entity_scene::SceneContribution> contributions;
        std::vector<lux::entity_scene::EntitySectionId> startup_sections;
        std::vector<EntitySectionCookInput> sections;
        std::vector<lux::entity_scene::RequiredExtension>
            required_extensions;
        /// Extra schemas required by contributions but absent from the
        /// Section images. Schemas present in an image are added automatically.
        std::vector<lux::entity_scene::RequiredComponentSchema>
            required_components;
    };

    struct CookedEntitySection final
    {
        lux::entity_scene::EntitySectionRecord record;
        lux::entity_scene::EntitySectionImage image;
        /// Canonical decoded LXES bytes. Stored sources publish these bytes;
        /// generated sources may retain them as deterministic expectation data.
        std::vector<std::byte> encoded_image;
    };

    /// Domain-neutral base result. Domain adapters may derive solely to append
    /// sidecar outputs (for example generated Mesh assets); the LXSC/LXES
    /// fields and cooker remain unaware of those domains.
    struct CookedEntitySceneBundle
    {
        lux::entity_scene::EntitySceneManifest manifest;
        std::vector<std::byte> encoded_manifest;
        /// Same UUID order as manifest.sections.
        std::vector<CookedEntitySection> sections;
    };

    /// Validates and encodes every canonical LXES image, derives record
    /// digests/counts/requirements, canonicalizes manifest collections, and
    /// finally encodes LXSC. Compression is deliberately outside this generic
    /// target; all records emitted here use NONE and exact encoded sizes.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<CookedEntitySceneBundle, EntitySceneCookFailure>
    cookEntityScene(EntitySceneCookInput input) noexcept;
}
