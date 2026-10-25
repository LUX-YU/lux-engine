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

    // A final color-output specification, not a Native resource or runtime owner.
    // Device limits, supported formats/sample counts and WSI eligibility are later checks.
    struct TargetDesc
    {
        std::uint32_t width{};
        std::uint32_t height{};
        rdesc::ETextureFormat format{rdesc::ETextureFormat::UNDEFINED};
        std::uint32_t samples{1};
        ETargetKind kind{ETargetKind::OFFSCREEN};
        bool sampled = false; // Explicit same-frame producer/consumer use; not an output history.

        [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void> validate() const noexcept;
        bool operator==(const TargetDesc&) const noexcept = default;
    };

    struct Win32Surface final
    {
        void* window = nullptr;
    };

    struct X11Surface final
    {
        void* display = nullptr;
        std::uint64_t window = 0;
    };

    struct WaylandSurface final
    {
        void* display = nullptr;
        void* surface = nullptr;
    };

    struct MetalSurface final
    {
        void* layer = nullptr;
    };

    using VSurfaceWindow = std::variant<std::monostate, Win32Surface, X11Surface, WaylandSurface, MetalSurface>;

    // Borrowed platform window, retained until RemoveTarget completes (or RenderServer joins).
    // No Window/UI object crosses the protocol. The first path is SDR; WAITABLE/HDR are not implied.
    struct SurfaceDescription final
    {
        VSurfaceWindow window;
        bool vertical_sync = true;
    };

    // Target's uint64_t epoch changes when its specification changes in place.
    // It is independent of TargetId::gen and is not stored here:
    // two identical descriptions need not belong to the same object or revision.
} // namespace lux::render
