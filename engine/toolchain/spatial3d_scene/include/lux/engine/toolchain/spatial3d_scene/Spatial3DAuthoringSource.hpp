#pragma once

#include <lux/engine/ecs/PersistentEntityId.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/math/Grid.hpp>
#include <lux/engine/toolchain/spatial3d_scene/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <uuid.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lux::toolchain
{
    enum class ESpatial3DSourceTopology : std::uint8_t
    {
        PLANAR_XZ,
        VOLUMETRIC_XYZ
    };

    struct Spatial3DSourceSpace final
    {
        uuids::uuid id{};
        ESpatial3DSourceTopology topology{
            ESpatial3DSourceTopology::PLANAR_XZ};
        double cell_edge{0.0};
    };

    struct Spatial3DNavigationAgentSource final
    {
        std::uint8_t profile_index{0u};
        float radius{0.5f};
        float height{1.8f};
        float maximum_climb{0.5f};
        float maximum_slope_degrees{45.0f};
        float cell_size{0.3f};
        float cell_height{0.2f};
    };

    /// Owning, 3D-only Authoring input for the LXSC/LXES cooker.  It carries
    /// content facts only: no World dimension, coordinate paging, streaming
    /// budget, environment singleton or activation switch survives here.
    struct Spatial3DActorComponentSource final
    {
        std::string schema_name;
        std::uint32_t schema_version{1u};
        std::vector<std::byte> tagged_payload;
    };

    struct Spatial3DActorSource final
    {
        lux::ecs::PersistentEntityId id;
        uuids::uuid space{};
        lux::math::Position3d position;
        std::optional<lux::ecs::PersistentEntityId> transform_parent;
        std::vector<std::string> data_layers;
        std::vector<std::byte> name_table;
        std::vector<Spatial3DActorComponentSource> components;
    };

    struct Spatial3DInstanceSource final
    {
        lux::ecs::PersistentEntityId id;
        lux::math::Position3d position;
        std::array<float, 4u> rotation{0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 3u> scale{1.0f, 1.0f, 1.0f};
        lux::asset::asset_id_t mesh{};
        lux::asset::asset_id_t material_instance{};
        std::uint32_t rgba8{0xffffffffu};
        std::uint64_t stable_pick_id{0u};
    };

    struct Spatial3DInstancePageSource final
    {
        uuids::uuid space{};
        lux::math::GridCoord3i64 cell;
        std::vector<std::string> data_layers;
        std::vector<Spatial3DInstanceSource> instances;
    };

    struct Spatial3DTerrainPageSource final
    {
        uuids::uuid terrain_set{};
        uuids::uuid space{};
        lux::math::GridCoord3i64 cell;
        float height_min{0.0f};
        float height_max{1.0f};
        float sample_spacing{1.0f};
        std::uint8_t weight_layer_count{0u};
        std::vector<float> heights;
        std::array<std::vector<std::uint8_t>, 2u> weight_planes;
        std::vector<std::uint8_t> holes;
    };

    struct Spatial3DAuthoringSource final
    {
        lux::scene::ScenePackageId scene;
        std::vector<lux::scene::SceneFeatureRequest> features;
        std::vector<Spatial3DSourceSpace> spaces;
        std::vector<lux::scene::RequiredExtension> required_extensions;
        std::vector<Spatial3DActorSource> actors;
        std::vector<Spatial3DInstancePageSource> instance_pages;
        std::vector<Spatial3DTerrainPageSource> terrain_pages;
        std::vector<Spatial3DNavigationAgentSource>
            navigation_agent_profiles;
    };

    struct Spatial3DAuthoringLoadLimits final
    {
        std::uint64_t maximum_actor_document_bytes{
            64ull * 1024ull * 1024ull};
    };

    /// Reads the LXWA v4 Authoring object graph into the owning, 3D-only
    /// source above. 2D spaces and their pages are deliberately ignored;
    /// mixed-dimension scenes can install a separate 2D cooker without either
    /// leaf knowing the other's source model.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_SPATIAL3D_SCENE_PUBLIC
    lux::cxx::expected<Spatial3DAuthoringSource, std::string>
    loadSpatial3DAuthoringSource(
        const std::filesystem::path& root_document,
        const Spatial3DAuthoringLoadLimits& limits = {}) noexcept;
} // namespace lux::toolchain
