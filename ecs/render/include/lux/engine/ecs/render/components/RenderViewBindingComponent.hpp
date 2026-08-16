#pragma once
// ============================================================================
//  RenderViewBindingComponent.hpp — 这台相机**拥有**的 view 句柄
//  (lux::ecs, kind-neutral; 用户裁定 2026-07-11，批 3 升级语义 2026-08-02)。
//
//  Cameras are cheap world data (many, freely created); views are deliberate
//  render-side resources (few: editor viewport, game window, previews). This
//  component is their ONLY intersection.
//  Upload paths iterate view<CameraCache, RenderViewBinding> — the sparse set
//  itself skips unbound cameras, so multi-camera / multi-view is pure data
//  composition with no per-camera branch, and two cameras bound to one view is
//  a detectable data error instead of a frame-order race.
//
//  ⚠️ 批 3 把语义从「指向别人拥有的 view 的指针」升级成「**持有**那个 view」；
//  批 6 又把这条语义落实为 move-only `RenderViewLease`，不再靠 on_destroy
//  回调读一个可复制句柄后手写 `removeView`。**摘掉组件 = 交还那个 view。**
//  只有 `CameraViewSubsystem` 该 emplace 它 —— 宿主要的是挂 `ViewPresentComponent`
//  (「这台相机要出图」),句柄由系统在 addView 回复落地时装上。
//
//  RUNTIME-ONLY: never reflected, never persisted (ViewHandle is a
//  generational slot key into the live render server)。三个相机组件的分工写在
//  `ViewPresentComponent.hpp` 头部,别在这里再抄一遍。
// ============================================================================

#include <lux/engine/function/render/client/RenderLease.hpp>

#include <utility>

namespace lux::ecs
{
    struct RenderViewBindingComponent
    {
        explicit RenderViewBindingComponent(
            lux::render::RenderViewLease lease) noexcept
            : lease_(std::move(lease))
        {
        }

        RenderViewBindingComponent(const RenderViewBindingComponent&) = delete;
        RenderViewBindingComponent& operator=(const RenderViewBindingComponent&) = delete;
        RenderViewBindingComponent(RenderViewBindingComponent&&) noexcept = default;
        RenderViewBindingComponent& operator=(RenderViewBindingComponent&&) noexcept = default;

        [[nodiscard]] lux::render::ViewHandle view() const noexcept
        {
            return lease_.id();
        }

        /// Explicit parent-before-child teardown hook. Ordinary component
        /// destruction still has the lease destructor as a non-blocking backstop.
        [[nodiscard]] lux::render::ERenderLeaseCloseStatus close() noexcept
        {
            return lease_.close();
        }

    private:
        lux::render::RenderViewLease lease_;
    };

} // namespace lux::ecs
