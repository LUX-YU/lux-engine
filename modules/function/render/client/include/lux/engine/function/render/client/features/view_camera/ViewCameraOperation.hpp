#pragma once
// ============================================================================
//  ViewCameraOperation.hpp — ViewCamera 通信外观的【作者声明】(A+)
//
//  3D 相机(view/proj 矩阵、相机位、派生视锥)是 FEATURE 域:核心协议不再
//  认识相机数据,省略 StandardViewCamera 的 2D/headless 场景不携带这套词汇。
//  Bulk 类:一次推送可携多视图更新(客户端逐视图发 span of 1)。
//
//  Operation 面由 engine_add_comm_ops 生成;手写残余 =
//  handleViewCameraUpdate 语义 + viewCameraUpdate 便捷自由函数
//  (render_client/ViewCameraOperation.cpp)。生成 Proxy 的 update 收
//  span<const Payload> wire 面;单视图 + 三段数组拷贝的便捷形状按 §7.5
//  留为自由函数 —— 31 个调用点只做文本改名,拷贝语义不挪家。
// ============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    /// 无客户端创建参数 —— 空配置结构只承载特性身份注解。
    struct LUX_COMM_CONFIG(prefix=ViewCamera, id=lux.render.view_camera.v1, display=StandardViewCamera,
                           feature=StandardViewCameraFeature,
                           feature_header=lux/engine/render/renderer/features/view_camera/StandardViewCameraFeature.hpp)
    ViewCameraCommTag
    {
    };
    static_assert(std::is_trivially_copyable_v<ViewCameraCommTag>);

    /// Per-view camera update(核心 RenderProtocol 退役件):column-major 4x4,
    /// Vulkan ZO 投影(NDC z ∈ [0,1])。
    struct LUX_OP(lane=frame, kind=bulk, name=ViewCameraUpdate, method=update)
    ViewCameraUpdatePayload
    {
        RenderSceneId scene_id{};
        ViewHandle    view{};
        float view_matrix[16]{};      // column-major 4x4
        float proj_matrix[16]{};      // column-major 4x4, Vulkan ZO (NDC z in [0,1])
        RenderLargePosition3D render_origin{};
        float coordinate_page_size{1024.0f};
    };
    static_assert(std::is_trivially_copyable_v<ViewCameraUpdatePayload>);

    class ViewCameraProxy;   // 生成于 comm/genops/ViewCameraOperation.ops.hpp

    /// 单视图便捷面(裸矩阵数组 → 载荷拷贝 → span of 1),原 Proxy::update
    /// 的形状降级为自由函数 —— 语义糖留手写,wire 面归生成。
    LUX_FUNCTION_PUBLIC void viewCameraUpdate(
        ViewCameraProxy proxy,
        RenderSceneId scene_id, ViewHandle view,
        const float view_matrix[16],
        const float proj_matrix[16],
        const RenderLargePosition3D& render_origin,
        float coordinate_page_size);

    /// Explicit page-zero adapter for render tests and other transient scenes.
    /// Persistent World callers must use the exact RenderLargePosition3D API.
    LUX_FUNCTION_PUBLIC void viewCameraUpdateTransient(
        ViewCameraProxy proxy,
        RenderSceneId scene_id, ViewHandle view,
        const float view_matrix[16],
        const float proj_matrix[16],
        const float camera_position[3],
        float coordinate_page_size = 1024.0f);

} // namespace lux::render
