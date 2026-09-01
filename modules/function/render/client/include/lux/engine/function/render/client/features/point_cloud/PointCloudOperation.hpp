#pragma once
// ============================================================================
//  PointCloudOperation.hpp — PointCloud 通信外观的【作者声明】(A+)
//
//  五种渲染模式(Simple/GPUDriven/LOD/Splatting/Transient)各有 CommConfig
//  与工厂,共享同一套 op/Proxy —— 多工厂形状走 no_factory:客户端面
//  (op 描述符/CommandTraits/OperationIds/Proxy)由 engine_add_comm_ops 生成,
//  五个 createFn/descriptor/factory 留手写(PCOperationHandlers.cpp,含
//  handle* 语义)。工厂 extern 因此也留在本头。
// ============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>
#include <lux/engine/function/visibility.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;

    // =========================================================================
    //  Default shader name constants for PointCloudFeature
    // =========================================================================
    inline constexpr std::string_view kPCSimpleVertShaderName = "pointcloud_simple.vert";
    inline constexpr std::string_view kPCSimpleFragShaderName = "pointcloud_simple.frag";
    inline constexpr std::string_view kPCCullingCompShaderName = "pointcloud_culling.comp";
    inline constexpr std::string_view kPCLodVertShaderName = "pointcloud_lod.vert";
    inline constexpr std::string_view kPCSplatFragShaderName = "pointcloud_splat.frag";

    /// 通信身份 tag:只为承载 prefix 与 no_factory(五模式共享一套 op,
    /// 工厂/描述符全手写)。不是 wire 类型。
    struct LUX_TYPE_INFO(both) LUX_COMM_CONFIG(prefix = PointCloud, no_factory = true) PointCloudCommTag
    {
    };

    // =========================================================================
    //  Per-mode CommConfig structs (trivially copyable, transferred as attachments)
    // =========================================================================
    struct LUX_TYPE_INFO(both) PCSimpleCommConfig
    {
        ShaderHandle vertex_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle fragment_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        float initial_point_size{3.0f};
        uint32_t max_global_points{4'000'000};
        uint32_t max_octree_nodes{65'536};
    };
    static_assert(std::is_trivially_copyable_v<PCSimpleCommConfig>);

    struct LUX_TYPE_INFO(both) PCGPUDrivenCommConfig
    {
        ShaderHandle compute_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle vertex_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle fragment_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        float initial_point_size{3.0f};
        uint32_t max_nodes{65'536};
    };
    static_assert(std::is_trivially_copyable_v<PCGPUDrivenCommConfig>);

    struct LUX_TYPE_INFO(both) PCLODCommConfig
    {
        ShaderHandle compute_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle vertex_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle fragment_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        float point_size_world{0.05f};
        float min_size{1.0f};
        float max_size{20.0f};
        uint32_t max_nodes{65'536};
    };
    static_assert(std::is_trivially_copyable_v<PCLODCommConfig>);

    struct LUX_TYPE_INFO(both) PCSplattingCommConfig
    {
        ShaderHandle compute_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle vertex_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle fragment_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        float point_size_world{0.05f};
        float min_size{2.0f};
        float max_size{30.0f};
        uint32_t max_nodes{65'536};
    };
    static_assert(std::is_trivially_copyable_v<PCSplattingCommConfig>);

    struct LUX_TYPE_INFO(both) PCTransientCommConfig
    {
        ShaderHandle vertex_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle fragment_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        float point_size{3.0f};
        uint32_t max_points{2'000'000};
    };
    static_assert(std::is_trivially_copyable_v<PCTransientCommConfig>);

    // =========================================================================
    //  Strongly-typed point(客户端便捷类型,语义糖)。16 字节 GPU 布局。
    // =========================================================================
    struct PointCloudPoint
    {
        float x, y, z;
        uint32_t packed_attr;

        static constexpr uint32_t pack(float r, float g, float b, float intensity = 1.0f) noexcept
        {
            auto to_u8 = [](float v) constexpr -> uint32_t {
                return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            return (to_u8(r) << 0) | (to_u8(g) << 8) | (to_u8(b) << 16) | (to_u8(intensity) << 24);
        }

        static constexpr PointCloudPoint
        make(float px, float py, float pz, float r, float g, float b, float intensity = 1.0f) noexcept
        {
            return {px, py, pz, pack(r, g, b, intensity)};
        }
    };
    static_assert(sizeof(PointCloudPoint) == 16, "PointCloudPoint must be 16 bytes to match GPU layout");

    // =========================================================================
    //  Reply
    // =========================================================================
    struct PointCloudChunkUploadedReply
    {
        uint32_t chunk_id{0};
        uint32_t status{0}; ///< 0 = success
    };
    static_assert(std::is_trivially_copyable_v<PointCloudChunkUploadedReply>);

    // =========================================================================
    //  Op payloads(声明序 = 注册序)
    // =========================================================================

    /// Upload (or replace) a point cloud chunk. Data is copied into the request payload.
    struct LUX_OP(
        lane = upload,
        kind = blob,
        name = PointCloudUpload,
        method = uploadChunk,
        reply = PointCloudChunkUploadedReply,
        opcode = resource) UploadPointCloudChunkPayload
    {
        RenderSceneId scene_id{};
        uint32_t chunk_id{0};
        uint32_t point_count{0}; ///< 必须等于 blob 字节数 / sizeof(PointCloudPoint)
        LUX_OP_BLOB() BlobRef point_data {};
    };
    static_assert(std::is_trivially_copyable_v<UploadPointCloudChunkPayload>);

    /// Remove a point cloud chunk from the GPU (fire-and-forget).
    struct LUX_OP(lane = control, kind = stream, name = PointCloudRemove, method = removeChunk)
        RemovePointCloudChunkPayload
    {
        RenderSceneId scene_id{};
        uint32_t chunk_id{0};
    };
    static_assert(std::is_trivially_copyable_v<RemovePointCloudChunkPayload>);

    /// Clear all point cloud chunks for a scene (free all slots + octree nodes).
    struct LUX_OP(
        lane = control,
        kind = resource,
        name = PointCloudClearAll,
        method = clearAll,
        reply = GenericOkReply,
        opcode = command) ClearAllPointCloudPayload
    {
        RenderSceneId scene_id{};
    };
    static_assert(std::is_trivially_copyable_v<ClearAllPointCloudPayload>);

    /// Clear a single chunk (reset point_count to 0, keep slot allocated).
    struct LUX_OP(
        lane = control,
        kind = resource,
        name = PointCloudClearChunk,
        method = clearChunk,
        reply = GenericOkReply,
        opcode = command) ClearPointCloudChunkPayload
    {
        RenderSceneId scene_id{};
        uint32_t chunk_id{0};
    };
    static_assert(std::is_trivially_copyable_v<ClearPointCloudChunkPayload>);

    /// Runtime point-size adjustment.
    /// Semantics per render mode:
    ///   - Simple / GPUDriven / Transient : screen-pixel size (assigned directly).
    ///   - LOD / Splatting                : screen-pixel upper clamp (lod_pc.max_size);
    ///                                      world radius and min clamp keep their
    ///                                      original Config values so depth scaling
    ///                                      is preserved.
    struct LUX_OP(lane = program, kind = stream, name = PointCloudSetPointSize, method = setPointSize)
        SetPointCloudPointSizePayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        float point_size{3.0f};
    };
    static_assert(std::is_trivially_copyable_v<SetPointCloudPointSizePayload>);

    // =========================================================================
    //  Per-mode FeatureFactory externs(no_factory:工厂留手写,extern 留此)
    // =========================================================================
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kPCFeatureSimpleFactory;
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kPCFeatureGPUDrivenFactory;
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kPCFeatureLODFactory;
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kPCFeatureSplattingFactory;
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kPCFeatureTransientFactory;
    extern LUX_RENDER_FEATURE_METADATA_PUBLIC const FeatureDescriptor kPCSimpleDescriptor;
    extern LUX_RENDER_FEATURE_METADATA_PUBLIC const FeatureDescriptor kPCGPUDrivenDescriptor;
    extern LUX_RENDER_FEATURE_METADATA_PUBLIC const FeatureDescriptor kPCLODDescriptor;
    extern LUX_RENDER_FEATURE_METADATA_PUBLIC const FeatureDescriptor kPCSplattingDescriptor;
    extern LUX_RENDER_FEATURE_METADATA_PUBLIC const FeatureDescriptor kPCTransientDescriptor;
    extern LUX_RENDER_FEATURE_METADATA_PUBLIC const RenderFeatureRegistration kPCSimpleRegistration;
    extern LUX_RENDER_FEATURE_METADATA_PUBLIC const RenderFeatureRegistration kPCGPUDrivenRegistration;
    extern LUX_RENDER_FEATURE_METADATA_PUBLIC const RenderFeatureRegistration kPCLODRegistration;
    extern LUX_RENDER_FEATURE_METADATA_PUBLIC const RenderFeatureRegistration kPCSplattingRegistration;
    extern LUX_RENDER_FEATURE_METADATA_PUBLIC const RenderFeatureRegistration kPCTransientRegistration;

} // namespace lux::render
