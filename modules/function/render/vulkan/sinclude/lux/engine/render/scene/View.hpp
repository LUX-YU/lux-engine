#pragma once
/*
 * View.hpp — Per-View Pure Data Container
 *
 * View is a plain data struct with no methods.  All lifecycle
 * (init, destroy, tick, resize, …) is driven by RenderScene.
 */
#include <lux/engine/function/render/client/core/RenderTypes.hpp> // lux::math::Extent2u + kViewDataStrideBytes
#include <lux/engine/render/core/FrustumCuller.hpp>               // exact-origin ViewCullData stride
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp> // ViewHandle (generational)
#include <lux/engine/render/gpu/utils/Slot.hpp>                     // SlotHandle
#include <lux/engine/function/visibility.h>
#include <lux/cxx/container/SparseSet.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lux::render
{
    // =========================================================================
    // ViewState — per-view lifecycle state
    // =========================================================================
    enum class ViewState : uint8_t
    {
        Created,    ///< Resources allocated, not yet rendered
        Active,     ///< Actively rendering each frame
        Paused,     ///< Resources allocated, skipping frames
        Destroying, ///< Pending resource release (deferred by frames-in-flight)
    };

    // =========================================================================
    // View — per-view pure data (all lifecycle driven by RenderScene)
    // =========================================================================

    struct LUX_FUNCTION_PUBLIC View
    {
        View();
        ~View();

        // ── Identity ────────────────────────────────────────────────────
        ViewHandle handle{}; ///< generational; .index keys per-view tables
        ViewState state{ViewState::Created};
        std::string debug_name;

        // ── Extent ──────────────────────────────────────────────────────
        /// 渲染期从 target binding 派生的缓存(renderSingleView 每次录制前
        /// 回写;首次渲染前 = addView 的初始值,供相机帧数据兜底)。视图
        /// 不再自持尺寸账本——改尺寸走 ResizeTarget 直达图像池。
        lux::math::Extent2u current_extent{};

        // ── Per-frame lifecycle ─────────────────────────────────────────
        bool frame_systems_done{false};

        // ── Per-view GPU data staging (domain-neutral) ───────────────────
        /// Opaque per-view bytes the StandardViewCamera feature publishes each
        /// frame (the GPU view-data SoA entry + the 6 cull-frustum planes).
        /// RenderScene::beginFrame uploads view_data_staging into the Set-0
        /// binding-1 slot AFTER the slot fence wait (#27 — the feature fills these
        /// CPU-side during the op dispatch; the GPU write happens at beginFrame).
        /// The core does NOT interpret the bytes — only the feature + shaders do.
        /// A camera-less view (no StandardViewCamera op) leaves has_view_data
        /// false → zero per-view 3D traffic.
        std::array<std::byte, kViewDataStrideBytes> view_data_staging{};
        std::array<std::byte, kViewFrustumStrideBytes> frustum_staging{};
        bool has_view_data{false};

        // ── SceneResources slot ──────────────────────────────────────────
        SlotHandle view_slot{}; ///< View SoA slot in SceneResources::ViewBuffer

        // ── Mesh instance tracking (object -> visibility metadata) ──────────
        struct VisibleObjectEntry
        {
            uint32_t bucket_id{0};
            uint32_t generation{0};
        };
        lux::cxx::OffsetSparseSet<uint32_t, VisibleObjectEntry> active_objects;

        void registerObject(RenderObjectHandle object, uint32_t bucket) noexcept
        {
            if (!object)
                return;
            active_objects.insert(object.index, VisibleObjectEntry{bucket, object.gen});
        }

        void unregisterObject(RenderObjectHandle object) noexcept
        {
            if (!object)
                return;
            active_objects.erase(object.index);
        }

        // ── Per-view render graph resource state ─────────────────────────
        /// Owns the physical GPU resources (VkImage/VkBuffer) and record context
        /// allocated for this view's extent.  nullptr until first render.
        std::unique_ptr<struct RGResourceState> resource_state;
    };
} // namespace lux::render
