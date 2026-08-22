#pragma once
/**
 * @file SceneDescription.hpp
 * @brief Engine-owned cooked scene description model (LXSC v2 semantics).
 *
 * EntitySection image layout belongs to ecs::scene_format. This package adds
 * Engine concerns around those images: derived extension/component
 * requirements, storage/generation recipes and startup policy.
 */

#include <lux/engine/extensions/ExtensionId.hpp>
#include <lux/engine/ecs/scene_format/SceneSectionManifest.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lux::scene
{
    inline constexpr std::uint32_t kSceneDescriptionMagic = 0x4353584cu;
    inline constexpr std::uint32_t kSceneDescriptionVersion = 2u;

    struct RequiredExtension final
    {
        lux::extensions::ExtensionId id;
        std::uint16_t required_major{0u};
        std::uint16_t minimum_minor{0u};

        friend bool operator==(const RequiredExtension&, const RequiredExtension&) = default;
    };

    struct SceneDescription final
    {
        lux::asset::asset_id_t                  id;
        /// Stable L3SC loading metadata for the Spatial3D ECS domain. Empty
        /// means that the Scene has no partitioned Spatial3D content. This is
        /// data consumed by streaming Systems, not a behavior selector.
        std::vector<std::byte>                  spatial3d_catalog;
        /// Derived by Cook from serialized Components and project renderer
        /// configuration. Required entries gate publication; optional entries
        /// only select a path when FeatureCatalog provides it.
        std::vector<std::string>                required_render_features;
        std::vector<std::string>                optional_render_features;
        std::vector<lux::ecs::scene_format::EntitySectionId> startup_sections;
        std::vector<lux::ecs::scene_format::SectionRecord> sections;
        std::vector<RequiredExtension>          required_extensions;
        std::vector<lux::ecs::scene_format::RequiredComponentSchema>
                                                required_components;

        friend bool operator==(const SceneDescription&, const SceneDescription&) = default;
    };
} // namespace lux::scene
