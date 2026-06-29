#include <lux/engine/ui/ImGuiDrawDataSnapshot.hpp>

#include <imgui_impl_vulkan.h>

#include <cstring>

// ImGui_ImplVulkan_ViewportData is defined in imgui_impl_vulkan.cpp (not exported).
// We only pass around pointers — the forward decl in the header is sufficient.
// The cast from void* (RendererUserData) to the forward-declared pointer type is valid.

namespace lux::ui
{
    // =============================================================================
    // ImDrawDataSnapshot
    // =============================================================================
    ImDrawDataSnapshot::ImDrawDataSnapshot() = default;

    ImDrawDataSnapshot::~ImDrawDataSnapshot()
    {
        clear();
    }

    void ImDrawDataSnapshot::clear()
    {
        for (int i = 0; i < cache_.Size; ++i)
            if (cache_[i].our_copy)
                IM_DELETE(cache_[i].our_copy);
        cache_.clear();
        owned_cmd_lists_.clear();
        draw_data_.Clear();
        viewport_events_.clear();
        releaseViewportFrames();
        viewport_frames_.clear();
    }

    void ImDrawDataSnapshot::releaseViewportFrames()
    {
        for (int i = 0; i < viewport_frames_.Size; ++i)
        {
            viewport_frames_[i].draw_data.CmdLists = {};      // free deep-copy alias
            viewport_frames_[i].owned_cmd_lists.clear();       // free canonical owner
        }
    }

    ImDrawDataSnapshot::CachedDrawList *ImDrawDataSnapshot::findOrAddEntry(ImDrawList *src)
    {
        for (int i = 0; i < cache_.Size; ++i)
            if (cache_[i].src_ptr == src)
                return &cache_[i];

        cache_.push_back({});
        return &cache_.back();
    }

    void ImDrawDataSnapshot::garbageCollect(double current_time)
    {
        const double threshold = current_time - kGcSeconds;
        for (int i = cache_.Size - 1; i >= 0; --i)
        {
            if (cache_[i].last_used > threshold)
                continue;
            if (cache_[i].our_copy)
                IM_DELETE(cache_[i].our_copy);
            cache_.erase(&cache_[i]);
        }
    }

    void ImDrawDataSnapshot::snapDrawDataInto(
        ImDrawData *src, ImDrawData &dst, ImVector<ImDrawList *> &dst_lists, double current_time)
    {
        // Copy top-level scalar fields but preserve dst_lists.
        ImVector<ImDrawList *> backup;
        backup.swap(dst_lists);
        dst = *src;
        backup.swap(dst_lists);

        dst_lists.resize(0);
        for (int i = 0; i < src->CmdListsCount; ++i)
        {
            ImDrawList *src_list = src->CmdLists[i];
            CachedDrawList *entry = findOrAddEntry(src_list);

            if (entry->our_copy == nullptr)
            {
                entry->src_ptr = src_list;
                entry->our_copy = IM_NEW(ImDrawList)(src_list->_Data);
            }

            entry->our_copy->CmdBuffer.swap(src_list->CmdBuffer);
            entry->our_copy->VtxBuffer.swap(src_list->VtxBuffer);
            entry->our_copy->IdxBuffer.swap(src_list->IdxBuffer);

            ImDrawCmdHeader tmp_header = entry->our_copy->_CmdHeader;
            entry->our_copy->_CmdHeader = src_list->_CmdHeader;
            src_list->_CmdHeader = tmp_header;

            entry->our_copy->Flags = src_list->Flags;
            entry->our_copy->_OwnerName = src_list->_OwnerName;

            src_list->CmdBuffer.reserve(entry->our_copy->CmdBuffer.Capacity);
            src_list->VtxBuffer.reserve(entry->our_copy->VtxBuffer.Capacity);
            src_list->IdxBuffer.reserve(entry->our_copy->IdxBuffer.Capacity);

            entry->last_used = current_time;
            dst_lists.push_back(entry->our_copy);
        }

        dst.CmdLists = dst_lists;
        dst.CmdListsCount = dst_lists.Size;
    }

    void ImDrawDataSnapshot::snap(ImDrawData *src)
    {
        IM_ASSERT(src != nullptr && src->Valid);

        const double current_time = ImGui::GetTime();

        // Drain viewport lifecycle events from the ImGui backend into this snapshot.
        int evt_count = 0;
        const auto* events = ImGui_ImplVulkan_DrainViewportEventsEx(&evt_count);
        viewport_events_.resize(evt_count);
        if (evt_count > 0)
            memcpy(viewport_events_.Data, events, evt_count * sizeof(ImGui_ImplVulkan_ViewportEvent));

        // Snapshot main viewport draw data.
        snapDrawDataInto(src, draw_data_, owned_cmd_lists_, current_time);

        // Snapshot secondary viewport draw data so the render thread never
        // needs to read platform_io.Viewports.
        releaseViewportFrames();
        viewport_frames_.resize(0);
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        for (int i = 1; i < pio.Viewports.Size; i++)
        {
            ImGuiViewport* vp = pio.Viewports[i];
            if (vp->Flags & ImGuiViewportFlags_IsMinimized)
                continue;
            auto* vd = (ImGui_ImplVulkan_ViewportData*)vp->RendererUserData;
            if (!vd)
                continue;

            viewport_frames_.push_back({});
            auto& entry = viewport_frames_.back();
            entry.vd    = vd;
            entry.size  = vp->Size;
            entry.flags = vp->Flags;

            if (vp->DrawData && vp->DrawData->Valid)
                snapDrawDataInto(vp->DrawData, entry.draw_data, entry.owned_cmd_lists, current_time);
        }

        // Periodically release cached draw lists that haven't been used recently.
        garbageCollect(current_time);
    }
} // namespace lux::ui
