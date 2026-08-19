#pragma once

#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>
#include <lux/engine/scene/ScenePackageCodec.hpp>

namespace lux::scene::detail
{
    [[nodiscard]] lux::entity_scene::EntitySceneCodecLimits legacyLimits(
        const ScenePackageCodecLimits& limits) noexcept;

    [[nodiscard]] ScenePackageCodecFailure packageFailure(
        const lux::entity_scene::EntitySceneCodecFailure& failure);

    [[nodiscard]] ScenePackageCodecResult<
        lux::entity_scene::EntitySectionRecord>
    toLegacySectionRecord(
        const SectionRecord& record,
        const ScenePackageCodecLimits& limits) noexcept;

    [[nodiscard]] ScenePackageCodecResult<
        lux::entity_scene::EntitySceneManifest>
    toLegacyManifest(
        const ScenePackage& package,
        const ScenePackageCodecLimits& limits) noexcept;

    [[nodiscard]] ScenePackage fromLegacyManifest(
        const lux::entity_scene::EntitySceneManifest& manifest);
} // namespace lux::scene::detail
