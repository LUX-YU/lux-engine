#pragma once
// ============================================================================
//  RGForwardDecls.hpp — Lightweight backend-neutral graph declarations
//
//  Lives in include/ so that public headers (RenderFeature.hpp) can
//  reference render-graph types without pulling in implementation headers.
// ============================================================================

#include <cstdint>
#include <limits>

namespace lux::render
{

    // ── Render-graph resource handle (value type) ────────────────────────

    struct RGResourceHandle
    {
        uint32_t index{std::numeric_limits<uint32_t>::max()};

        constexpr bool isInvalid() const
        {
            return index == std::numeric_limits<uint32_t>::max();
        }

        explicit operator bool() const
        {
            return index != std::numeric_limits<uint32_t>::max();
        }
    };

    inline constexpr RGResourceHandle invalid_rg_resource_handle = RGResourceHandle{};

    // ── Ping-pong / history resource handle pair ─────────────────────────
    //  current() is written this frame; previous()/history(k) read an earlier
    //  frame's copy of the SAME logical resource. The two handles map to one
    //  physical copy set, offset by frame phase.
    struct RGRingResourceHandle
    {
        RGResourceHandle cur{};
        RGResourceHandle prev{};

        [[nodiscard]] RGResourceHandle current()  const noexcept { return cur; }
        [[nodiscard]] RGResourceHandle previous() const noexcept { return prev; }
    };

    // ── Pass kernel identifiers ──────────────────────────────────────────
    //  Opaque numeric ID assigned by KernelRegistry at static-init time.
    //  0 is reserved as kInvalidKernelId (no kernel / recorder fallback).
    //  IDs are allocated sequentially per KernelRegistry::registerKernel() call.
    //  Lives here (public, light) so KernelDescriptor.hpp can be authored from
    //  the install tree without pulling the heavy RGPassTypes.hpp.
    //  Upgrade path: change to uint16_t here — everything else follows.
    using KernelTypeId = uint8_t;

    /// Sentinel value meaning "no kernel registered" (recorder lambda path).
    inline constexpr KernelTypeId kInvalidKernelId = 0;

    // ── Forward declarations ─────────────────────────────────────────────

    class RGBuilder;
    struct RGCompiledGraph;
    struct RGRecordContext;

} // namespace lux::render
