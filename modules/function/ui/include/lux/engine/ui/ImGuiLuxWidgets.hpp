#pragma once
// ============================================================================
//  ImGuiLuxWidgets.hpp — Sentinel-based ImGui widgets for deferred texture resolution
//
//  These widgets encode scene view IDs or texture handles into sentinel
//  ImTextureID values.  The render thread resolves these sentinels to real
//  VkDescriptorSets before calling ImGui_ImplVulkan_RenderDrawData.
// ============================================================================

#include <lux/engine/function/visibility_ui.h>
#include <lux/engine/render/core/RenderResourceHandle.hpp>
#include <lux/engine/render/core/RenderSceneId.hpp>

#include <imgui.h>

#include <cassert>
#include <cstdint>
#include <utility>

namespace lux::ui
{

    // ════════════════════════════════════════════════════════════════════
    //  Sentinel bit layout (ImTextureID is ImU64):
    //
    //    Bit 63:     1 = scene view sentinel
    //    Bit 62:     1 = texture handle sentinel
    //    Scene-view payload:
    //      Bits 62-31: scene_id (32 bits, full range)
    //      Bits 30-0 : view_id  (31 bits)
    //    Texture payload:
    //      Bits 31-0  : handle index+gen
    //
    //  Valid VkDescriptorSets are heap pointers and never have bit 63/62 set.
    // ════════════════════════════════════════════════════════════════════

    inline constexpr ImU64 kSceneViewSentinelBit     = ImU64(1) << 63;
    inline constexpr ImU64 kTextureHandleSentinelBit  = ImU64(1) << 62;
    inline constexpr ImU64 kFontAtlasSentinelBit      = ImU64(1) << 61;

    // ── Encoding ─────────────────────────────────────────────────────────

    inline ImTextureID encodeSceneViewSentinel(
        lux::render::RenderSceneId scene_id, uint32_t view_id) noexcept
    {
        assert((view_id & 0x80000000u) == 0 &&
               "SceneView sentinel supports up to 31-bit view_id");
        // scene_id is a generational SlotKey (index+gen); the ImTextureID sentinel
        // has only 31 bits for it, so — as before the handle became generational —
        // we pack the slot INDEX only. decodeSceneView reconstructs {index, gen=0}.
        return static_cast<ImTextureID>(
            kSceneViewSentinelBit
            | (static_cast<ImU64>(scene_id.index) << 31)
            | (static_cast<ImU64>(view_id) & 0x7FFFFFFFu));
    }

    inline ImTextureID encodeTextureHandleSentinel(
        lux::render::RTextureHandle handle) noexcept
    {
        return static_cast<ImTextureID>(
            kTextureHandleSentinelBit
            | (static_cast<ImU64>(handle.index) << 16)
            | static_cast<ImU64>(handle.gen));
    }

    // ── Decoding ─────────────────────────────────────────────────────────

    inline bool isSceneViewSentinel(ImTextureID id) noexcept
    {
        return (static_cast<ImU64>(id) & kSceneViewSentinelBit) != 0;
    }

    inline bool isTextureHandleSentinel(ImTextureID id) noexcept
    {
        return (static_cast<ImU64>(id) & kTextureHandleSentinelBit) != 0;
    }

    inline bool isFontAtlasSentinel(ImTextureID id) noexcept
    {
        return (static_cast<ImU64>(id) & kFontAtlasSentinelBit) != 0;
    }

    inline ImTextureID encodeFontAtlasSentinel() noexcept
    {
        return static_cast<ImTextureID>(kFontAtlasSentinelBit);
    }

    inline bool isSentinel(ImTextureID id) noexcept
    {
        return isSceneViewSentinel(id) || isTextureHandleSentinel(id) || isFontAtlasSentinel(id);
    }

    inline std::pair<lux::render::RenderSceneId, uint32_t>
    decodeSceneView(ImTextureID id) noexcept
    {
        ImU64 raw = static_cast<ImU64>(id);
        uint32_t scene_val = static_cast<uint32_t>((raw >> 31) & 0xFFFFFFFFu);
        uint32_t view_id   = static_cast<uint32_t>(raw & 0x7FFFFFFFu);
        return { lux::render::RenderSceneId{scene_val}, view_id };
    }

    inline lux::render::RTextureHandle decodeTextureHandle(ImTextureID id) noexcept
    {
        ImU64 raw = static_cast<ImU64>(id);
        uint32_t index = static_cast<uint32_t>((raw >> 16) & 0xFFFF);
        uint16_t gen   = static_cast<uint16_t>(raw & 0xFFFF);
        return lux::render::RTextureHandle{index, gen};
    }

    // NOTE: the `SceneView()` / `ImageFromHandle()` convenience widgets were
    // removed — they had no callers (panels build the sentinel ImTextureID via
    // encodeSceneViewSentinel / encodeTextureHandleSentinel above and pass it to
    // ImGui::Image directly), and their definitions lived in an un-built .cpp.

} // namespace lux::ui
