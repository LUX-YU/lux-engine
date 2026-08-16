#pragma once
/**
 * @file SpawnRegistry.hpp
 * @brief Registry of entity-creation recipes behind the editor's Create menu
 *        (Hierarchy `+` / right-click, scene-viewport right-click).
 *
 * The Create menu is DATA, not a hardcoded item list: each
 * `SpawnRecipe` declares its menu placement (label + category), its scene
 * gating (one required scene contribution), and the spawn function itself.
 * Built-ins are seeded by
 * `LuxEditor::init`; packs/plugins append their own recipes through the same
 * `add()` — no editor-code edits (the feature/importer-registry pattern).
 *
 * The spawn function mutates the live World directly (the established
 * Inspector-Add-Component idiom: panel paints run on the main thread between
 * World ticks, so direct registry mutation is safe); it returns the created
 * entity so the caller can select it.
 */

#include <lux/engine/resource/spatial/Spatial.hpp>

#include <Eigen/Core>
#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lux::ecs { class World; }

namespace lux::editor
{
    /// Everything a recipe may need at spawn time. Positions are optional —
    /// present when the creation came from a viewport right-click ("create
    /// HERE"); recipes fall back to the origin / their own default otherwise.
    struct SpawnContext
    {
        lux::ecs::World& world;
        std::optional<lux::spatial::Position2D> pos2d;
        std::optional<lux::spatial::Position3D> pos3d;
    };

    struct SpawnRecipe
    {
        std::string id;         ///< stable identifier ("empty", "image2d", ...)
        std::string label;      ///< menu label ("Empty", "Image", ...)
        std::string category;   ///< "" = top-level item; else a submenu ("2D", "3D")

        /// Empty means universally available. Otherwise the exact LXSC/LXWA
        /// contribution must be selected; no top-level scene kind shadows it.
        std::string required_contribution;

        std::function<entt::entity(const SpawnContext&)> spawn;
    };

    /// Owned by LuxEditor (one per editor process). Registration order is the
    /// menu order within a category.
    class SpawnRegistry
    {
    public:
        void add(SpawnRecipe recipe) { recipes_.push_back(std::move(recipe)); }

        [[nodiscard]] std::span<const SpawnRecipe> all() const noexcept
        { return recipes_; }

    private:
        std::vector<SpawnRecipe> recipes_;
    };

    /// Seed the built-in recipes (Empty / 2D Image / 2D Camera / 3D Camera /
    /// 3D lights) into @p registry. Called once at editor init; packs/plugins
    /// append theirs through the same add(). Defined in SpawnRecipes.cpp —
    /// the recipes are registry DATA, not editor-shell behavior.
    void registerBuiltinSpawnRecipes(SpawnRegistry& registry);

} // namespace lux::editor
