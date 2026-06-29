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
//  TargetSlot — identifies a well-known output slot in a RenderTarget
// ─────────────────────────────────────────────────────────────────────

enum class TargetSlot : uint8_t
{
    SceneColor  = 0,   ///< Primary colour output (LDR or HDR backbuffer)
    SceneDepth  = 1,   ///< Primary depth/stencil output
    ResolveColor = 2,  ///< MSAA resolve target
    COUNT              ///< Sentinel — keep last
};

static constexpr size_t kTargetSlotCount = static_cast<size_t>(TargetSlot::COUNT);

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
