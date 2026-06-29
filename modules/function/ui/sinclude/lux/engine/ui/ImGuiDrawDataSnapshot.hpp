#pragma once

#include <lux/engine/function/visibility_ui.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <cstdint>

struct ImGui_ImplVulkan_ViewportData; // forward decl (defined in imgui_impl_vulkan.cpp)

namespace lux::ui
{
    /// Per-secondary-viewport snapshot: everything the render thread needs.
    struct ViewportFrameEntry
    {
        ImGui_ImplVulkan_ViewportData* vd = nullptr;
        ImDrawData                     draw_data{};
        ImVector<ImDrawList*>          owned_cmd_lists;
        ImVec2                         size{};
        ImGuiViewportFlags             flags = 0;
    };

    /**
     * @brief Deep-snapshot of ImDrawData for cross-thread rendering.
     *
     * Inspired by imgui_club/imgui_threaded_rendering.h (MIT license).
     * Uses a swap-based approach: buffers are exchanged (not copied) between
     * the source ImDrawList and our cached copy, making each snap() O(DrawListCount)
     * pointer swaps instead of O(VtxCount + IdxCount) memcpy.
     *
     * Thread safety: NOT thread-safe on its own.  Use external
     * synchronisation (e.g. FramePack attachment) for cross-thread access.
     */
    struct LUX_FUNCTION_UI_PUBLIC ImDrawDataSnapshot
    {
        ImDrawDataSnapshot();
        ~ImDrawDataSnapshot();

        ImDrawDataSnapshot(const ImDrawDataSnapshot &) = delete;
        ImDrawDataSnapshot &operator=(const ImDrawDataSnapshot &) = delete;

        /**
         * @brief Take a snapshot by swapping buffers with src's draw lists.
         *
         * After this call the snapshot holds the complete draw data and src's
         * draw list buffers are replaced with previously cached (reusable)
         * buffers so that ImGui can reuse them without reallocation.
         *
         * Also snapshots all secondary viewport draw data so the render
         * thread never needs to read platform_io.Viewports.
         *
         * Must be called on the thread that owns the ImGui context.
         */
        void snap(ImDrawData *src);

        /// Clear all cached draw lists and free memory.
        void clear();

        /// Access the snapshotted draw data (valid after snap() until next snap() or clear()).
        [[nodiscard]] ImDrawData &drawData() noexcept { return draw_data_; }
        [[nodiscard]] const ImDrawData &drawData() const noexcept { return draw_data_; }

        /// Access viewport lifecycle events captured during snap().
        [[nodiscard]] const ImVector<ImGui_ImplVulkan_ViewportEvent>& viewportEvents() const noexcept { return viewport_events_; }

        /// Access snapshotted secondary viewport frames for render-thread iteration.
        [[nodiscard]] const ImVector<ViewportFrameEntry>& viewportFrames() const noexcept { return viewport_frames_; }

    private:
        /// Free heap buffers owned by each ViewportFrameEntry.
        /// ImVector::resize(0)/clear() won't call destructors, so we must
        /// explicitly release draw_data.CmdLists and owned_cmd_lists.
        void releaseViewportFrames();

        struct CachedDrawList
        {
            ImDrawList *src_ptr = nullptr;  ///< Key: original ImDrawList pointer identity
            ImDrawList *our_copy = nullptr; ///< Our owned copy with swapped buffers
            double last_used = 0.0;
        };

        ImDrawData draw_data_{};
        ImVector<ImDrawList *> owned_cmd_lists_; ///< draw_data_.CmdLists storage
        ImVector<CachedDrawList> cache_;
        ImVector<ImGui_ImplVulkan_ViewportEvent> viewport_events_;
        ImVector<ViewportFrameEntry> viewport_frames_;
        static constexpr double kGcSeconds = 20.0; ///< Purge unused entries after this many seconds

        CachedDrawList *findOrAddEntry(ImDrawList *src);
        void snapDrawDataInto(ImDrawData *src, ImDrawData &dst, ImVector<ImDrawList *> &dst_lists, double current_time);
        void garbageCollect(double current_time);
    };

} // namespace lux::ui
