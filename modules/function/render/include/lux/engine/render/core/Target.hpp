#pragma once

#include <lux/engine/render/visibility.h>

#include <cstdint>
#include <lux/engine/description/Image.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <variant>

namespace lux::render
{
    enum class ETargetKind : std::uint8_t
    {
        OFFSCREEN,
        SURFACE
    };

    // Immutable Effect output contract. Actual pixel dimensions belong to Target and Instance.
    struct EffectOutputDesc final
    {
        rdesc::ETextureFormat   format{rdesc::ETextureFormat::UNDEFINED};
        std::uint32_t           samples{1};
        ETargetKind             kind{ETargetKind::OFFSCREEN};
        bool                    sampled{false};
        bool operator==(const EffectOutputDesc&) const noexcept = default;
    };

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void> validateEffectOutput(const EffectOutputDesc& output) noexcept;

    // A final color-output specification, not a Native resource or runtime owner.
    // Device limits, supported formats/sample counts and WSI eligibility are later checks.
    struct TargetDesc
    {
        std::uint32_t           width{};
        std::uint32_t           height{};
        rdesc::ETextureFormat   format{rdesc::ETextureFormat::UNDEFINED};
        std::uint32_t           samples{1};
        ETargetKind             kind{ETargetKind::OFFSCREEN};
        bool                    sampled = false; // Allows graph consumers to borrow the execution layer's published output.

        [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void> validate() const noexcept;
        bool                    operator==(const TargetDesc&) const noexcept = default;
    };

    struct Win32Surface final
    {
        void* window = nullptr;
        bool operator==(const Win32Surface&) const noexcept = default;
    };

    struct X11Surface final
    {
        void* display = nullptr;
        std::uint64_t window = 0;
        bool operator==(const X11Surface&) const noexcept = default;
    };

    struct WaylandSurface final
    {
        void* display = nullptr;
        void* surface = nullptr;
        bool operator==(const WaylandSurface&) const noexcept = default;
    };

    struct MetalSurface final
    {
        void* layer = nullptr;
        bool operator==(const MetalSurface&) const noexcept = default;
    };

    using VSurfaceWindow = std::variant<
        std::monostate,
        Win32Surface,
        X11Surface,
        WaylandSurface,
        MetalSurface
    >;

    // Borrowed platform window, retained until RemoveTarget completes (or RenderServer joins).
    // No Window/UI object crosses the protocol. The first path is SDR; WAITABLE/HDR are not implied.
    struct SurfaceDescription final
    {
        VSurfaceWindow window;
        bool vertical_sync = true;
        bool operator==(const SurfaceDescription&) const noexcept = default;
    };

    // Target's uint64_t epoch changes when its specification changes in place.
    // It is independent of TargetId::gen and is not stored here:
    // two identical descriptions need not belong to the same object or revision.
} // namespace lux::render
