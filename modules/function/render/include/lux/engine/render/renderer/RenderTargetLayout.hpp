#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

namespace lux::render
{

// ─────────────────────────────────────────────────────────────────────
//  TargetSlot — a well-known OUTPUT SEMANTIC a render target can expose
// ─────────────────────────────────────────────────────────────────────
//
// TargetSlot IS the output-semantic identity (阶段4): each value names WHAT an
// output means, not a fixed hardware register. The first three are the primary
// present/compose targets the framework injects (swapchain / offscreen pool). The
// rest are additional semantics a feature may render AND expose for external
// read-back / binding — editor picking (InstanceId), thumbnails, robotics
// multi-sensor (LinearDepth / Normal / SemanticClass). A slot is absent from a
// layout unless a feature declares it, so adding values here is zero-cost for
// scenes that don't use them (the per-slot arrays just grow by a few empty entries).
//
// This stays a closed enum on purpose: a multi-purpose game engine's output
// semantics are a known, finite set — a string-hashed open id (draft §7) would be
// YAGNI here. Widen this enum when a new built-in semantic is genuinely needed.

enum class TargetSlot : uint8_t
{
    // ── Primary present/compose targets (framework-injected) ──
    SceneColor    = 0,   ///< Primary colour output (LDR or HDR backbuffer)
    SceneDepth    = 1,   ///< Primary depth/stencil output
    ResolveColor  = 2,   ///< MSAA resolve target

    // ── Additional output semantics (feature-declared, externally accessible) ──
    Normal        = 3,   ///< world/view-space normals
    InstanceId    = 4,   ///< per-pixel instance id (editor picking / selection)
    LinearDepth   = 5,   ///< linearised depth (sensors / SSAO / DoF)
    SemanticClass = 6,   ///< semantic-segmentation class id
    MotionVector  = 7,   ///< screen-space motion (TAA / motion blur / optical flow)

    COUNT                ///< Sentinel — keep last
};

static constexpr size_t kTargetSlotCount = static_cast<size_t>(TargetSlot::COUNT);

/// Stable graph-resource name for an output-semantic slot. A feature writes an
/// additional semantic output by referencing this name (referenceTexture), and the
/// framework imports the slot under it (see RenderScene::compileGraphTemplate). Stable
/// across builds so name-based discovery is deterministic. (阶段4)
[[nodiscard]] inline const char* targetSlotName(TargetSlot s) noexcept
{
    switch (s)
    {
    case TargetSlot::SceneColor:    return "SceneColor";
    case TargetSlot::SceneDepth:    return "SceneDepth";
    case TargetSlot::ResolveColor:  return "ResolveColor";
    case TargetSlot::Normal:        return "SceneNormal";
    case TargetSlot::InstanceId:    return "InstanceId";
    case TargetSlot::LinearDepth:   return "LinearDepth";
    case TargetSlot::SemanticClass: return "SemanticClass";
    case TargetSlot::MotionVector:  return "MotionVector";
    default:                        return "UnknownOutput";
    }
}

// ─────────────────────────────────────────────────────────────────────
//  RenderTargetSlotDesc — static description of one output slot
// ─────────────────────────────────────────────────────────────────────

/// Describes the *format* and *usage* intent for a single slot.
/// Does NOT hold live image handles — those live in RenderTargetBinding.
struct RenderTargetSlotDesc
{
    VkFormat             format         = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags    usage          = 0;
    VkImageAspectFlags   aspect         = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout        initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;  ///< Expected layout on entry (UNDEFINED = fresh / don't care)
    VkImageLayout        final_layout   = VK_IMAGE_LAYOUT_UNDEFINED;
    bool                 is_presentable = false;
    bool                 preserve_content = false;  ///< If true, first loadOp is LOAD (not CLEAR) — for overlay-on-top scenarios

    /// Value equality — every field is part of the graph-compile identity, so a
    /// change in any of them must force a recompile. (Defaulted: all members are
    /// scalars / Vk enums.)
    friend bool operator==(const RenderTargetSlotDesc&, const RenderTargetSlotDesc&) = default;
};

// ─────────────────────────────────────────────────────────────────────
//  RenderTargetLayout — static graph-compile-time description
// ─────────────────────────────────────────────────────────────────────

/// Describes which slots a render target exposes and their properties.
/// Used at graph-compile time (compileGraphTemplate).
/// Does NOT change per-frame.
struct RenderTargetLayout
{
    std::array<std::optional<RenderTargetSlotDesc>, kTargetSlotCount> slots{};

    [[nodiscard]] bool hasSlot(TargetSlot s) const noexcept
    {
        return slots[static_cast<size_t>(s)].has_value();
    }

    [[nodiscard]] const RenderTargetSlotDesc& slot(TargetSlot s) const noexcept
    {
        assert(hasSlot(s) && "RenderTargetLayout: slot not present");
        return *slots[static_cast<size_t>(s)];
    }

    /// Full-layout value equality (compares every slot's presence + descriptor).
    /// The compiled graph template is only valid for the exact layout it was built
    /// from; Renderer::prepareSceneForRender uses this to detect a changed target
    /// layout and force a recompile. (A stable hash + per-key template cache for
    /// simultaneous heterogeneous layouts is 阶段4 — see .internal/UNFINISHED-WORK.md.)
    friend bool operator==(const RenderTargetLayout&, const RenderTargetLayout&) = default;
};

// ─────────────────────────────────────────────────────────────────────
//  SlotImages — per-FIF image + view handles for one slot
// ─────────────────────────────────────────────────────────────────────

struct SlotImages
{
    std::vector<VkImage>     images; ///< one per frame-in-flight
    std::vector<VkImageView> views;  ///< one per frame-in-flight
};

// ─────────────────────────────────────────────────────────────────────
//  RenderTargetBinding — per-frame runtime binding
// ─────────────────────────────────────────────────────────────────────
/// Pairs a RenderTargetLayout with live image handles for the current frame.
/// Must match the layout used to compile the graph template.
/// Lifetime: valid for one frame only.
struct RenderTargetBinding
{
    const RenderTargetLayout*                          layout{nullptr};
    std::array<SlotImages, kTargetSlotCount>           slot_images{};
    VkExtent2D                                         extent{};
    bool                                               is_presentable{false};

    [[nodiscard]] const SlotImages& slot(TargetSlot s) const noexcept
    {
        return slot_images[static_cast<size_t>(s)];
    }
};

} // namespace lux::render
