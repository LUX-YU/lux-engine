#pragma once

#include <lux/engine/description/Image.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace lux::render
{
    enum class TargetSlot : std::uint8_t
    {
        SCENE_COLOR = 0,
        SCENE_DEPTH,
        RESOLVE_COLOR,
        NORMAL,
        INSTANCE_ID,
        LINEAR_DEPTH,
        SEMANTIC_CLASS,
        MOTION_VECTOR,
        COUNT
    };

    inline constexpr std::size_t kTargetSlotCount =
        static_cast<std::size_t>(TargetSlot::COUNT);

    [[nodiscard]] inline const char* targetSlotName(TargetSlot slot) noexcept
    {
        switch (slot)
        {
        case TargetSlot::SCENE_COLOR:    return "SceneColor";
        case TargetSlot::SCENE_DEPTH:    return "SceneDepth";
        case TargetSlot::RESOLVE_COLOR:  return "ResolveColor";
        case TargetSlot::NORMAL:         return "SceneNormal";
        case TargetSlot::INSTANCE_ID:    return "InstanceId";
        case TargetSlot::LINEAR_DEPTH:   return "LinearDepth";
        case TargetSlot::SEMANTIC_CLASS: return "SemanticClass";
        case TargetSlot::MOTION_VECTOR:  return "MotionVector";
        case TargetSlot::COUNT:          break;
        }
        return "UnknownOutput";
    }

    enum class ERenderImageUsage : std::uint32_t
    {
        NONE                     = 0,
        COLOR_ATTACHMENT         = 1u << 0u,
        DEPTH_STENCIL_ATTACHMENT = 1u << 1u,
        SAMPLED                  = 1u << 2u,
        STORAGE                  = 1u << 3u,
        TRANSFER_SOURCE          = 1u << 4u,
        TRANSFER_DESTINATION     = 1u << 5u,
    };

    [[nodiscard]] constexpr ERenderImageUsage operator|(
        ERenderImageUsage lhs,
        ERenderImageUsage rhs
    ) noexcept
    {
        return static_cast<ERenderImageUsage>(
            static_cast<std::uint32_t>(lhs) |
            static_cast<std::uint32_t>(rhs)
        );
    }

    constexpr ERenderImageUsage& operator|=(
        ERenderImageUsage& lhs,
        ERenderImageUsage rhs
    ) noexcept
    {
        lhs = lhs | rhs;
        return lhs;
    }

    [[nodiscard]] constexpr bool hasUsage(
        ERenderImageUsage value,
        ERenderImageUsage flag
    ) noexcept
    {
        return (
            static_cast<std::uint32_t>(value) &
            static_cast<std::uint32_t>(flag)
        ) != 0u;
    }

    enum class ERenderAspect : std::uint8_t
    {
        COLOR,
        DEPTH,
        STENCIL,
        DEPTH_STENCIL,
    };

    enum class ERenderResourceState : std::uint8_t
    {
        UNDEFINED,
        COLOR_ATTACHMENT,
        DEPTH_STENCIL_ATTACHMENT,
        DEPTH_STENCIL_READ_ONLY,
        SHADER_READ,
        TRANSFER_SOURCE,
        TRANSFER_DESTINATION,
        PRESENT,
    };

    struct RenderTargetSlotDesc
    {
        lux::rdesc::ETextureFormat format{
            lux::rdesc::ETextureFormat::UNDEFINED};
        ERenderImageUsage    usage{ERenderImageUsage::NONE};
        ERenderAspect        aspect{ERenderAspect::COLOR};
        ERenderResourceState initial_state{ERenderResourceState::UNDEFINED};
        ERenderResourceState final_state{ERenderResourceState::UNDEFINED};
        bool                 is_presentable{false};
        bool                 preserve_content{false};

        friend bool operator==(
            const RenderTargetSlotDesc&,
            const RenderTargetSlotDesc&
        ) = default;
    };

    struct RenderTargetLayout
    {
        std::array<
            std::optional<RenderTargetSlotDesc>,
            kTargetSlotCount
        > slots{};

        [[nodiscard]] bool hasSlot(TargetSlot slot) const noexcept
        {
            return slots[static_cast<std::size_t>(slot)].has_value();
        }

        [[nodiscard]] const RenderTargetSlotDesc& slot(
            TargetSlot slot
        ) const noexcept
        {
            if (!hasSlot(slot))
                renderFatal(
                    "RenderTargetLayout::slot requires a populated slot");
            return *slots[static_cast<std::size_t>(slot)];
        }

        friend bool operator==(
            const RenderTargetLayout&,
            const RenderTargetLayout&
        ) = default;
    };

    [[nodiscard]] inline bool formatKeyEquals(
        const RenderTargetLayout& lhs,
        const RenderTargetLayout& rhs
    ) noexcept
    {
        for (std::size_t index = 0; index < kTargetSlotCount; ++index)
        {
            const auto& lhs_slot = lhs.slots[index];
            const auto& rhs_slot = rhs.slots[index];
            if (lhs_slot.has_value() != rhs_slot.has_value())
                return false;
            if (!lhs_slot)
                continue;
            if (
                lhs_slot->format != rhs_slot->format ||
                lhs_slot->aspect != rhs_slot->aspect ||
                lhs_slot->initial_state != rhs_slot->initial_state ||
                lhs_slot->preserve_content != rhs_slot->preserve_content
            )
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline RenderTargetSlotDesc defaultTargetSlotDesc(
        TargetSlot slot
    ) noexcept
    {
        RenderTargetSlotDesc desc{};
        desc.usage = ERenderImageUsage::COLOR_ATTACHMENT |
                     ERenderImageUsage::SAMPLED;
        desc.aspect = ERenderAspect::COLOR;
        desc.final_state = ERenderResourceState::SHADER_READ;
        switch (slot)
        {
        case TargetSlot::LINEAR_DEPTH:
            desc.format = lux::rdesc::ETextureFormat::R32_SFLOAT;
            break;
        case TargetSlot::NORMAL:
            desc.format = lux::rdesc::ETextureFormat::RGBA16_SFLOAT;
            break;
        case TargetSlot::MOTION_VECTOR:
            desc.format = lux::rdesc::ETextureFormat::RG16_SFLOAT;
            break;
        case TargetSlot::INSTANCE_ID:
            desc.format = lux::rdesc::ETextureFormat::R32_UINT;
            break;
        case TargetSlot::SEMANTIC_CLASS:
            desc.format = lux::rdesc::ETextureFormat::R16_UINT;
            break;
        default:
            break;
        }
        return desc;
    }

    [[nodiscard]] inline std::uint32_t targetSlotMask(
        const RenderTargetLayout& layout
    ) noexcept
    {
        std::uint32_t mask = 0;
        for (std::size_t index = 0; index < kTargetSlotCount; ++index)
        {
            if (layout.hasSlot(static_cast<TargetSlot>(index)))
                mask |= 1u << index;
        }
        return mask;
    }

    static_assert(std::is_trivially_copyable_v<RenderTargetSlotDesc>);
} // namespace lux::render
