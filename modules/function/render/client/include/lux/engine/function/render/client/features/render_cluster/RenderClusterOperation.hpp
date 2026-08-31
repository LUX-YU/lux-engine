#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;
    inline constexpr std::uint32_t kMaximumRenderClusterChildren = 16u;

    struct RenderClusterWireId final
    {
        std::uint8_t bytes[16]{};

        [[nodiscard]] bool valid() const noexcept
        {
            for (const auto value : bytes)
                if (value != 0u)
                    return true;
            return false;
        }

        friend bool operator==(const RenderClusterWireId&, const RenderClusterWireId&) = default;
    };
    static_assert(std::is_trivially_copyable_v<RenderClusterWireId>);

    struct alignas(16) RenderClusterWireInstance final
    {
        RenderSpatialTransform3D transform{};
        RMeshHandle mesh{};
        RMaterialHandle material{};
        std::uint64_t stable_pick_id{0u};
        std::uint32_t rgba8{0xffffffffu};
        std::uint32_t flags{kInstanceFlagCastShadow | kInstanceFlagReceiveShadow | kInstanceFlagVisible};
    };
    static_assert(std::is_trivially_copyable_v<RenderClusterWireInstance>);
    static_assert(sizeof(RenderClusterWireInstance) == 96u);

    struct RenderClusterUploadedReply final
    {
        RenderClusterWireId id;
        std::uint32_t instance_count{0u};
        std::uint32_t status{0u};
    };
    static_assert(std::is_trivially_copyable_v<RenderClusterUploadedReply>);

    struct LUX_OP(
        lane = upload,
        kind = resource,
        name = RenderClusterUpload,
        method = upload,
        reply = RenderClusterUploadedReply,
        opcode = resource) UploadRenderClusterPayload final
    {
        RenderSceneId scene_id{};
        RenderClusterWireId id;
        std::uint64_t revision{0u};
        std::uint32_t reserved0{0u};
        RenderLargePosition3D bounds_center{};
        float bounds_radius{0.0f};
        float lod_error{0.0f};
        std::uint32_t transition_milliseconds{350u};
        float hlod_enter_error_pixels{2.5f};
        float hlod_exit_error_pixels{1.5f};
        RenderClusterWireId parent;
        std::uint8_t lod_level{0u};
        std::uint8_t child_count{0u};
        std::uint16_t reserved1{0u};
        RenderClusterWireId children[kMaximumRenderClusterChildren]{};
        std::uint32_t instance_count{0u};
        ExternalDataRef instances{};
    };
    static_assert(std::is_trivially_copyable_v<UploadRenderClusterPayload>);

    struct RenderClusterRemovedReply final
    {
        RenderClusterWireId id;
        std::uint64_t revision{0u};
        std::uint32_t status{0u};
        std::uint32_t reserved{0u};
    };
    static_assert(std::is_trivially_copyable_v<RenderClusterRemovedReply>);

    struct LUX_OP(
        lane = control,
        kind = resource,
        name = RenderClusterRemove,
        method = remove,
        reply = RenderClusterRemovedReply,
        opcode = command) RemoveRenderClusterPayload final
    {
        RenderSceneId scene_id{};
        RenderClusterWireId id;
        std::uint64_t revision{0u};
    };
    static_assert(std::is_trivially_copyable_v<RemoveRenderClusterPayload>);

    struct RenderClusterStatsReply final
    {
        std::uint32_t cluster_count{0u};
        std::uint32_t instance_count{0u};
        std::uint32_t visible_cluster_count{0u};
        std::uint32_t visible_instance_count{0u};
        std::uint32_t gpu_candidate_count{0u};
        std::uint32_t gpu_candidate_count_valid{0u};
        std::uint32_t wanted_mip_texture_count{0u};
        std::uint32_t minimum_wanted_mip{0xffffffffu};
        std::uint32_t wanted_mip_feedback_valid{0u};
        std::uint32_t gpu_candidate_group_count{0u};
        std::uint32_t workgroup_aggregation_fallback_count{0u};
        // Progressive CPU-side validation of the same immutable instance
        // records consumed by the view-cull shader. Sampled only by the explicit
        // stats query (never per frame), so benchmark reports can distinguish a
        // candidate-dispatch failure from a property/frustum/MDC rejection.
        std::uint32_t cull_visible_flag_count{0u};
        std::uint32_t cull_gbuffer_pass_count{0u};
        std::uint32_t cull_geometry_count{0u};
        std::uint32_t cull_lod_count{0u};
        std::uint32_t cull_mdc_count{0u};
        std::uint32_t cull_frustum_count{0u};
        std::uint32_t non_white_instance_count{0u};
        std::uint32_t instance_rgba8_xor{0u};
        std::uint64_t full_texture_bytes{0u};
        std::uint64_t target_texture_bytes{0u};
        std::uint64_t actual_texture_bytes{0u};
        std::uint32_t gpu_candidate_requested_count{0u};
        std::uint32_t gpu_candidate_overflow_count{0u};
        std::uint64_t cpu_capacity_bytes{0u};
        std::uint32_t cpu_allocation_count{0u};
    };
    static_assert(std::is_trivially_copyable_v<RenderClusterStatsReply>);

    struct LUX_OP(
        lane = control,
        kind = resource,
        name = RenderClusterStats,
        method = stats,
        reply = RenderClusterStatsReply,
        opcode = command) QueryRenderClusterStatsPayload final
    {
        RenderSceneId scene_id{};
    };
    static_assert(std::is_trivially_copyable_v<QueryRenderClusterStatsPayload>);

    enum class ERenderPickStatus : std::uint32_t
    {
        PENDING,
        HIT,
        MISS,
        STALE,
        FAILED
    };

    /// Non-blocking pick intent. Coordinates are normalized to the current
    /// View content rect. request_generation is client-owned and monotonically
    /// increasing; view_generation rejects results from a recreated View.
    struct LUX_OP(lane = program, kind = stream, name = RenderClusterPickRequest, method = requestPick)
        RequestRenderClusterPickPayload final
    {
        RenderSceneId scene_id{};
        std::uint32_t view_index{0u};
        std::uint32_t view_generation{0u};
        std::uint64_t request_generation{0u};
        float normalized_x{0.5f};
        float normalized_y{0.5f};
        float maximum_distance{1'000'000.0f};
        std::uint32_t reserved{0u};
    };
    static_assert(std::is_trivially_copyable_v<RequestRenderClusterPickPayload>);

    struct RenderClusterPickReply final
    {
        std::uint64_t request_generation{0u};
        std::uint64_t stable_pick_id{0u};
        std::uint32_t view_generation{0u};
        ERenderPickStatus status{ERenderPickStatus::STALE};
        float depth{0.0f};
        std::uint32_t reserved{0u};
    };
    static_assert(std::is_trivially_copyable_v<RenderClusterPickReply>);

    struct LUX_OP(
        lane = control,
        kind = resource,
        name = RenderClusterPickResult,
        method = pickResult,
        reply = RenderClusterPickReply,
        opcode = command) QueryRenderClusterPickPayload final
    {
        RenderSceneId scene_id{};
        std::uint64_t request_generation{0u};
    };
    static_assert(std::is_trivially_copyable_v<QueryRenderClusterPickPayload>);

    struct LUX_COMM_CONFIG(
        prefix = RenderCluster,
        id = lux.render.cluster.v1,
        display = RenderCluster,
        requires = lux.render.mesh_stack.v1,
        feature = RenderClusterFeature,
        feature_header = lux / engine / render / renderer / features / render_cluster / RenderClusterFeature.hpp,
        multiplicity = single) RenderClusterCommTag
    {
    };
    static_assert(std::is_trivially_copyable_v<RenderClusterCommTag>);
} // namespace lux::render
