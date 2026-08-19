#pragma once
/**
 * @file EntitySceneCatalog.hpp
 * @brief Scene-scoped immutable owner of one validated LXSC manifest.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/entity_scene/EntityScene.hpp>
#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>
#include <lux/engine/scene/SceneFeatureId.hpp>

#include <span>
#include <utility>

namespace lux::runtime::entity_scene
{
    /// The sole runtime owner of decoded EntityScene metadata. Domain
    /// contributions borrow records from this SceneService; none of them
    /// builds a parallel manifest or copies the Section catalog.
    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC EntitySceneCatalog final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            EntitySceneCatalog,
            lux::entity_scene::EntitySceneCodecFailure>
        create(lux::entity_scene::EntitySceneManifest manifest) noexcept;

        EntitySceneCatalog(EntitySceneCatalog&&) noexcept = default;
        EntitySceneCatalog& operator=(EntitySceneCatalog&&) noexcept =
            default;
        EntitySceneCatalog(const EntitySceneCatalog&) = delete;
        EntitySceneCatalog& operator=(const EntitySceneCatalog&) = delete;

        [[nodiscard]] const lux::entity_scene::EntitySceneManifest& manifest()
            const noexcept
        {
            return manifest_;
        }

        [[nodiscard]] std::span<
            const lux::entity_scene::EntitySectionRecord>
        sections() const noexcept
        {
            return manifest_.sections;
        }

        [[nodiscard]] const lux::entity_scene::EntitySectionRecord* findSection(
            lux::entity_scene::EntitySectionId id) const noexcept;

        [[nodiscard]] const lux::entity_scene::SceneContribution*
        findContribution(
            lux::scene::SceneFeatureIdView id) const noexcept;

    private:
        explicit EntitySceneCatalog(
            lux::entity_scene::EntitySceneManifest manifest) noexcept
            : manifest_(std::move(manifest))
        {}

        lux::entity_scene::EntitySceneManifest manifest_;
    };
}
