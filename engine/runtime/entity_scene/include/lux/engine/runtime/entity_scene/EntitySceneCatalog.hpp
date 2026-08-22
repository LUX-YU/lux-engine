#pragma once
/**
 * @file EntitySceneCatalog.hpp
 * @brief Scene-scoped immutable owner of one validated Engine SceneDescription.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>

#include <span>
#include <utility>

namespace lux::runtime::entity_scene
{
    /// The sole runtime owner of decoded scene-package metadata. Runtime
    /// loading code borrows immutable Section records from this catalog; the
    /// historical Resource EntityScene DTO is confined to SceneDescription's
    /// private compatibility adapter.
    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC EntitySceneCatalog final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            EntitySceneCatalog,
            lux::scene::SceneCodecFailure>
        create(lux::scene::SceneDescription package) noexcept;

        EntitySceneCatalog(EntitySceneCatalog&&) noexcept = default;
        EntitySceneCatalog& operator=(EntitySceneCatalog&&) noexcept =
            default;
        EntitySceneCatalog(const EntitySceneCatalog&) = delete;
        EntitySceneCatalog& operator=(const EntitySceneCatalog&) = delete;

        [[nodiscard]] const lux::scene::SceneDescription& package() const noexcept
        {
            return package_;
        }

        [[nodiscard]] std::span<const lux::scene::SectionRecord> sections()
            const noexcept
        {
            return package_.sections;
        }

        [[nodiscard]] const lux::scene::SectionRecord* findSection(
            lux::ecs::scene_format::EntitySectionId id) const noexcept;

    private:
        explicit EntitySceneCatalog(lux::scene::SceneDescription package) noexcept
            : package_(std::move(package))
        {}

        lux::scene::SceneDescription package_;
    };
} // namespace lux::runtime::entity_scene
