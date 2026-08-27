#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;

    inline constexpr std::uint32_t kTerrainWireQuadEdge = 256u;
    inline constexpr std::uint32_t kTerrainWireSampleEdge = 257u;
    inline constexpr std::uint32_t kTerrainWirePatchEdge = 32u;
    inline constexpr std::uint32_t kTerrainWireMinMaxNodeCount = 87381u;

    struct TerrainWireId final
    {
        std::uint8_t bytes[16]{};

        [[nodiscard]] bool valid() const noexcept
        {
            for (const auto value : bytes)
                if (value != 0u)
                    return true;
            return false;
        }

        friend bool operator==(const TerrainWireId&, const TerrainWireId&) = default;
    };
    static_assert(std::is_trivially_copyable_v<TerrainWireId>);

    /// Header at the start of UploadTerrainPagePayload::page_data. Arrays are
    /// tightly concatenated in this order: R16 height, RGBA8 weight plane 0,
    /// RGBA8 weight plane 1, Hole bytes, uint16 min/max pairs, R16 fallback.
    struct TerrainWirePageDataHeader final
    {
        std::uint32_t height_count{0u};
        std::uint32_t weight_plane_bytes{0u};
        std::uint32_t hole_bytes{0u};
        std::uint32_t min_max_node_count{0u};
        std::uint32_t fallback_height_count{0u};
        std::uint32_t reserved[3]{};
    };
    static_assert(std::is_trivially_copyable_v<TerrainWirePageDataHeader>);
    static_assert(sizeof(TerrainWirePageDataHeader) == 32u);

    struct TerrainPageUploadedReply final
    {
        TerrainWireId id;
        std::uint64_t revision{0u};
        std::uint32_t cache_slot{0xffffffffu};
        std::uint32_t status{0u};
    };
    static_assert(std::is_trivially_copyable_v<TerrainPageUploadedReply>);

    struct LUX_OP(
        lane = upload,
        kind = resource,
        name = TerrainPageUpload,
        method = upload,
        reply = TerrainPageUploadedReply,
        opcode = resource) UploadTerrainPagePayload final
    {
        RenderSceneId scene_id{};
        TerrainWireId id;
        TerrainWireId parent;
        TerrainWireId children[16]{};
        std::uint64_t revision{0u};
        RenderLargePosition3D origin{};
        float height_min{0.0f};
        float height_max{1.0f};
        float sample_spacing{1.0f};
        float geometric_error{0.0f};
        float hlod_enter_error_pixels{2.5f};
        float hlod_exit_error_pixels{1.5f};
        std::uint32_t transition_milliseconds{350u};
        std::uint32_t transition_seed{1u};
        std::uint8_t hierarchy_level{0u};
        std::uint8_t child_count{0u};
        std::uint8_t weight_layer_count{0u};
        std::uint8_t reserved{0u};
        ExternalDataRef page_data{};
    };
    static_assert(std::is_trivially_copyable_v<UploadTerrainPagePayload>);

    struct TerrainPageRemovedReply final
    {
        TerrainWireId id;
        std::uint64_t revision{0u};
        std::uint32_t status{0u};
        std::uint32_t reserved{0u};
    };
    static_assert(std::is_trivially_copyable_v<TerrainPageRemovedReply>);

    struct LUX_OP(
        lane = control,
        kind = resource,
        name = TerrainPageRemove,
        method = remove,
        reply = TerrainPageRemovedReply,
        opcode = command) RemoveTerrainPagePayload final
    {
        RenderSceneId scene_id{};
        TerrainWireId id;
        std::uint64_t revision{0u};
    };
    static_assert(std::is_trivially_copyable_v<RemoveTerrainPagePayload>);

    struct TerrainPageCacheStatsReply final
    {
        std::uint32_t resident_pages{0u};
        std::uint32_t full_resolution_pages{0u};
        std::uint32_t fallback_pages{0u};
        std::uint32_t capacity_pages{0u};
        std::uint32_t gpu_unavailable_pages{0u};
        std::uint32_t fallback_capacity_pages{0u};
        std::uint32_t selected_patch_count{0u};
        std::uint32_t selected_patch_count_valid{0u};
        std::uint32_t fine_pages{0u};
        std::uint32_t hlod_pages{0u};
        std::uint32_t drawable_pages{0u};
        std::uint32_t drawable_pages_by_level[5]{};
        std::uint32_t transition_pages{0u};
        std::uint32_t debug_view_surface_valid{0u};
        std::uint32_t debug_view_surface_level{0u};
        std::int32_t debug_view_page_delta[3]{};
        float debug_view_local[3]{};
        float debug_view_surface_height{0.0f};
        float debug_view_surface_clearance{0.0f};
        std::uint64_t cpu_resident_bytes{0u};
        std::uint64_t gpu_resident_bytes{0u};
        std::uint64_t gpu_capacity_bytes{0u};
    };
    static_assert(std::is_trivially_copyable_v<TerrainPageCacheStatsReply>);

    struct LUX_OP(
        lane = control,
        kind = resource,
        name = TerrainPageCacheStats,
        method = stats,
        reply = TerrainPageCacheStatsReply,
        opcode = command) QueryTerrainPageCacheStatsPayload final
    {
        RenderSceneId scene_id{};
    };
    static_assert(std::is_trivially_copyable_v<QueryTerrainPageCacheStatsPayload>);

    struct LUX_COMM_CONFIG(
        prefix = Terrain,
        id = lux.render.terrain.v1,
        display = Terrain,
        requires = "lux.render.view_camera.v1,lux.render.deferred_gbuffer.v1",
        feature = TerrainFeature,
        feature_header = lux / engine / render / renderer / features / terrain / TerrainFeature.hpp,
        multiplicity = single) TerrainCommConfig
    {
        /// Logical full-resolution page budget. Parent fallback pages use a
        /// separate bounded cache owned by TerrainResources.
        std::uint32_t page_capacity{128u};
        /// Hard per-view GPU expansion budget. This is a platform/render
        /// budget, not World content identity, and may therefore differ
        /// between desktop and mobile builds.
        std::uint32_t maximum_selected_patches{1024u};
        float wanted_radius{2048.0f};
        std::uint64_t demotion_delay_frames{120u};
    };
    static_assert(std::is_trivially_copyable_v<TerrainCommConfig>);
} // namespace lux::render
