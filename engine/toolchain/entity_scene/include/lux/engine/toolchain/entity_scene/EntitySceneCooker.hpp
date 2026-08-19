#pragma once
/**
 * @file EntitySceneCooker.hpp
 * @brief Generic, domain-neutral ScenePackage/LXES cook construction.
 *
 * EntitySection images belong to ecs::scene_format. Scene selection, required
 * extensions and Section source recipes belong to Engine ScenePackage. The
 * cooker deliberately exposes only those canonical owners; the legacy LXSC v1
 * wire model remains private to scene_package's compatibility codec.
 */

#include <lux/engine/toolchain/entity_scene/EntitySceneCookError.hpp>

#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/engine/scene/ScenePackage.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <vector>

namespace lux::toolchain
{
    struct EntitySectionCookInput final
    {
        lux::ecs::scene_format::EntitySectionImage image;
        lux::scene::SectionSource source;
        std::vector<lux::ecs::scene_format::EntitySectionId> dependencies;
        std::vector<lux::scene::DemandChannelId> demand_channels;
        std::vector<lux::scene::RequiredExtension> required_extensions;
    };

    struct ScenePackageCookInput final
    {
        lux::scene::ScenePackageId id;
        std::vector<lux::scene::SceneFeatureRequest> features;
        std::vector<lux::ecs::scene_format::EntitySectionId> startup_sections;
        std::vector<EntitySectionCookInput> sections;
        std::vector<lux::scene::RequiredExtension> required_extensions;
        /// Extra schemas required by Scene Features but absent from Section
        /// images. Schemas present in an image are added automatically.
        std::vector<lux::scene::RequiredComponentSchema> required_components;
    };

    struct CookedEntitySection final
    {
        lux::scene::SectionRecord record;
        lux::ecs::scene_format::EntitySectionImage image;
        /// Canonical decoded LXES bytes. Stored sources publish these bytes;
        /// generated sources may retain them as deterministic expectation data.
        std::vector<std::byte> encoded_image;
    };

    /// Domain-neutral base result. Domain adapters may derive solely to append
    /// sidecar outputs (for example generated Mesh assets); the ScenePackage
    /// and LXES fields remain unaware of those domains.
    struct CookedScenePackageBundle
    {
        lux::scene::ScenePackage package;
        std::vector<std::byte> encoded_package;
        /// Same UUID order as package.sections.
        std::vector<CookedEntitySection> sections;
    };

    /// Validates and encodes every canonical LXES image, derives Section record
    /// digests/counts/requirements, canonicalizes package collections, and
    /// finally encodes LXSC v1 through the Engine-owned ScenePackage codec.
    /// Compression is deliberately outside this generic target; all records
    /// emitted here use SectionCompression::None and exact encoded sizes.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ENTITY_SCENE_PUBLIC
    lux::cxx::expected<CookedScenePackageBundle, EntitySceneCookFailure>
    cookScenePackage(ScenePackageCookInput input) noexcept;
} // namespace lux::toolchain
