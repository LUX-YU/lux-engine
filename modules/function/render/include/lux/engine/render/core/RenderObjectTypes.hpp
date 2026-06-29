#pragma once

#include <compare>
#include <cstdint>

namespace lux::render
{

    enum class EGeometryKind : uint8_t
    {
        StaticMesh = 0,
        SkinnedMesh,
        Particle,
        Decal,
        Custom,
    };

    enum EPassBit : uint16_t
    {
        eBasePass      = 1u << 0,
        eShadow        = 1u << 1,
        eGBuffer       = 1u << 2,
        eDepthPrepass  = 1u << 3,
        eEmissive      = 1u << 4,
        eTransparent   = 1u << 5,
    };

    using PassMask = uint16_t;

    inline constexpr PassMask operator|(EPassBit a, EPassBit b) noexcept
    {
        return static_cast<PassMask>(a) | static_cast<PassMask>(b);
    }

    inline constexpr PassMask operator|(PassMask mask, EPassBit bit) noexcept
    {
        return static_cast<PassMask>(mask | static_cast<PassMask>(bit));
    }

    inline constexpr bool hasPass(PassMask mask, EPassBit bit) noexcept
    {
        return (mask & static_cast<PassMask>(bit)) != 0;
    }

    inline constexpr PassMask kPassMaskOpaqueDefault =
        static_cast<PassMask>(eBasePass | eGBuffer | eDepthPrepass | eShadow);

    // (The ERenderFlag enum that lived here is gone — it was dead code with zero
    //  consumers. Per-instance render flags are the kInstanceFlag* bits in
    //  RenderProtocol.hpp; the dead MotionVector/EditorOnly bits went with it.)

    struct RenderObjectHandle
    {
        uint32_t index{~0u};
        uint32_t gen{0};

        [[nodiscard]] static constexpr RenderObjectHandle invalid() noexcept { return {}; }
        [[nodiscard]] explicit constexpr operator bool() const noexcept { return index != ~0u; }
        friend auto operator<=>(RenderObjectHandle, RenderObjectHandle) = default;
    };

} // namespace lux::render
