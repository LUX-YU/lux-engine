#pragma once
// ============================================================================
//  ViewPresentComponent.hpp
//
//  用户裁定(2026-08-02):**相机 → view 是部分的,view → 相机是全的**。
//  不是所有相机实体都要有 view;但每一个可见的 view 都需要一台相机。
//  这个组件就是那个「要」——挂上它,CameraViewSubsystem 就为该实体建一个 view 并
//  合成到 target;摘掉它(或销毁实体),view 被还回去。
//
//  三个相机组件各答一个问题,别混:
//
//    PrimaryCameraTag            存盘   「这个场景发布出去,该用哪台相机」
//    ViewPresentComponent        运行期 「此刻哪台相机要出图,出到哪」   ← 本文件
//    RenderViewBindingComponent  运行期 「这台相机拥有的 view 句柄」(系统产物)
//
//  ⚠️ `PrimaryCameraTag` **不触发任何渲染动作**。它是内容作者的意图；
//  presentation contribution 在 owner safe point 将它翻译为本组件。
//
//  RUNTIME-ONLY:永不反射、永不存盘。`target` 是宿主拥有的输出容器(编辑器的
//  offscreen / 游戏的 surface),句柄跨进程无意义。
// ============================================================================

#include <lux/engine/function/render/client/core/FeatureHandle.hpp>   // RenderTargetId
#include <lux/engine/common/Size2D.hpp>

#include <cstdint>

namespace lux::ecs
{
    struct ViewPresentComponent
    {
        /// 合成到哪个输出容器。宿主创建并拥有它（编辑器 offscreen / 游戏 surface）。
        lux::render::RenderTargetId target{};

        /// 该 target 上的层序。今天恒为 0（一个 target 一路图）；多路合成时才有意义。
        std::uint32_t order{0};

        /// 视口尺寸。两个用途，别只记住第一个：
        ///   ① `addView` 的创建尺寸；
        ///   ② **相机 aspect 的来源** —— 所以窗口/视口改尺寸时宿主要
        ///      `registry.patch<ViewPresentComponent>()` 更新它，否则画面会拉伸。
        ///      （view 自身的渲染尺寸随 target 派生，不用重建 view。）
        lux::common::Size2D extent{};
    };

} // namespace lux::ecs
