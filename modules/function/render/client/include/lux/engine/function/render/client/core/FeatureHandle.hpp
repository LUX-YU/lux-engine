#pragma once
/**
 * @file FeatureHandle.hpp
 * @brief Strong-typed handles for render features and views.
 */

#include <cstdint>
#include <compare>
#include <limits>
#include <lux/cxx/container/SlotMap.hpp>   // lux::cxx::SlotKey

namespace lux::render
{
    /// Tag for the generational feature handle.
    struct FeatureTag {};

    /// Generational feature handle (index + generation). A reused feature slot
    /// bumps its generation, so a stale FeatureHandle is rejected by the scene's
    /// SlotKeyAutoSparseSet lookup instead of aliasing a different feature.
    /// Trivially copyable → serializes across the in-process comm channel (8 bytes).
    /// Members: .index / .gen / .valid().
    using FeatureHandle = lux::cxx::SlotKey<FeatureTag>;

    /// Tag for the generational view handle.
    struct ViewTag {};

    /// Generational view handle (index + generation). A reused view slot bumps its
    /// generation, so a stale ViewHandle (held by a client across a view
    /// remove+recreate) is rejected by the scene's SlotKeyAutoSparseSet lookup
    /// instead of aliasing a different view. Trivially copyable → serializes across
    /// the in-process comm channel (8 bytes). Members: .index / .gen / .valid().
    using ViewHandle = lux::cxx::SlotKey<ViewTag>;

    /// Tag for the generational render-target handle.
    struct RenderTargetTag {};

    /// 一等渲染目标句柄(RenderTarget 一等化设计 §1):Surface(窗口宿主的
    /// 呈现目标)与 Offscreen(图像目标)统一寻址。生成式:target 销毁后
    /// 复用槽位会 bump generation,过期句柄被查找拒绝。Trivially copyable
    /// → 直接跨 comm 序列化(8 字节)。Members: .index / .gen / .valid()。
    using RenderTargetId = lux::cxx::SlotKey<RenderTargetTag>;
} // namespace lux::render
