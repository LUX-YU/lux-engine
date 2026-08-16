#pragma once
// ============================================================================
//  ImGuiLuxWidgets.hpp — Sentinel-based ImGui widgets for deferred texture resolution
//
//  These widgets encode render-target ids or texture handles into sentinel
//  ImTextureID values.  The render thread resolves these sentinels to real
//  VkDescriptorSets before calling ImGui_ImplVulkan_RenderDrawData.
// ============================================================================

#include <lux/engine/function/visibility_ui.h>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>          // RenderTargetId
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>

#include <imgui.h>

#include <cassert>
#include <cstdint>
#include <utility>

namespace lux::ui
{

    // ════════════════════════════════════════════════════════════════════
    //  Sentinel bit layout (ImTextureID is ImU64):
    //
    //    Bit 63:     1 = render-target sentinel
    //    Bit 62:     1 = texture handle sentinel
    //    Bit 61:     1 = font atlas sentinel
    //    Render-target payload (generational RenderTargetId):
    //      Bits 60-31: target gen   (low 30 bits)
    //      Bits 30-0 : target index (31 bits)
    //    Texture payload:
    //      Bits 31-0  : handle index+gen
    //
    //  Valid VkDescriptorSets are heap pointers and never have bit 63/62/61
    //  set; payloads stay below bit 61 so the tag tests never alias.
    // ════════════════════════════════════════════════════════════════════

    inline constexpr ImU64 kRenderTargetSentinelBit   = ImU64(1) << 63;
    inline constexpr ImU64 kTextureHandleSentinelBit  = ImU64(1) << 62;
    inline constexpr ImU64 kFontAtlasSentinelBit      = ImU64(1) << 61;

    // ── Encoding ─────────────────────────────────────────────────────────

    inline ImTextureID encodeRenderTargetSentinel(
        lux::render::RenderTargetId target) noexcept
    {
        assert((target.index & 0x80000000u) == 0 &&
               "RenderTarget sentinel supports up to 31-bit target index");
        assert((target.gen & 0xC0000000u) == 0 &&
               "RenderTarget sentinel supports up to 30-bit target gen");
        return static_cast<ImTextureID>(
            kRenderTargetSentinelBit
            | ((static_cast<ImU64>(target.gen) & 0x3FFFFFFFu) << 31)
            | (static_cast<ImU64>(target.index) & 0x7FFFFFFFu));
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

    inline bool isRenderTargetSentinel(ImTextureID id) noexcept
    {
        return (static_cast<ImU64>(id) & kRenderTargetSentinelBit) != 0;
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

    inline lux::render::RenderTargetId decodeRenderTarget(ImTextureID id) noexcept
    {
        ImU64 raw = static_cast<ImU64>(id);
        return lux::render::RenderTargetId{
            static_cast<uint32_t>(raw & 0x7FFFFFFFu),
            static_cast<uint32_t>((raw >> 31) & 0x3FFFFFFFu)};
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
    // encodeRenderTargetSentinel / encodeTextureHandleSentinel above and pass it to
    // ImGui::Image directly), and their definitions lived in an un-built .cpp.

} // namespace lux::ui
