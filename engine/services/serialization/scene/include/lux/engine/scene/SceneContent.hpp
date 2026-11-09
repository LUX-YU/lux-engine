#pragma once

#include <lux/engine/scene/SceneData.hpp>
#include <lux/engine/services/serialization/scene/visibility.h>
#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <span>

namespace lux::world { class World; }
namespace lux::ecs { class ComponentSchemaSet; }

namespace lux::scene
{
    // All codecs produce a separate candidate; failure preserves caller-owned output and metadata.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC cxx::expected<void, error::Error>
    encodeSceneSummary(
        std::span<const SceneObjectSummary> summaries, std::vector<std::byte>& output, std::size_t limit
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC cxx::expected<std::vector<SceneObjectSummary>, error::Error>
    decodeSceneSummary(std::span<const std::byte> bytes, std::size_t limit) noexcept;
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC cxx::expected<void, error::Error>
    encodeSceneBlock(
        std::span<const SceneObjectData> objects, std::span<const SceneObjectSummary> summaries,
        std::vector<std::byte>& output, SceneBlockDescription& description, std::size_t limit
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC cxx::expected<std::vector<SceneObjectData>, error::Error>
    decodeSceneBlock(
        std::span<const std::byte> bytes, const SceneBlockDescription& description, std::size_t limit
    ) noexcept;

    // Main structural safe point only. Entities must already have persistent World identities.
    // Runtime-derived fields are omitted; non-null Entity links are rejected. No Registry borrow escapes the call.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC cxx::expected<void, error::Error>
    captureSceneBlock(
        world::World& world, std::span<const ecs::Entity> entities, const ecs::ComponentSchemaSet& schemas,
        std::vector<SceneObjectData>& output
    ) noexcept;
    // Prepare all values before publishing any entity. Missing schemas or Entity links reject the whole group.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC cxx::expected<std::vector<ecs::Entity>, error::Error>
    restoreSceneGroup(
        world::World& world, std::span<const SceneObjectData> objects, const ecs::ComponentSchemaSet& schemas
    ) noexcept;
} // namespace lux::scene
