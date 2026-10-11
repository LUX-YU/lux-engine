#pragma once

#include <optional>

#include <lux/engine/render/core/Handles.hpp>
#include <lux/engine/render/core/Identity.hpp>

namespace lux::render
{
    struct RuntimePathTag;
    using RuntimePathId = cxx::StrongId<RuntimePathTag, std::uint32_t, 0>;

    // Slot keys are local to their owning Runtime endpoint. A foreign endpoint
    // must reject the owner identity even when index/generation happen to match.
    template <typename Local> struct RuntimeAddress
    {
        std::uint64_t owner;
        Local local;
        bool operator==(const RuntimeAddress&) const noexcept = default;
    };

    using RuntimePath = RuntimeAddress<RuntimePathId>;
    using RuntimeScene = RuntimeAddress<RenderSceneId>;
    using RuntimeView = RuntimeAddress<RenderViewHandle>;
    using RuntimeTarget = RuntimeAddress<RenderTargetId>;

    struct RuntimeTargetRect
    {
        std::uint32_t x{}, y{}, width{}, height{};
    };

    // Ordered opaque output layers. Empty rectangle covers the complete target
    // and follows resize. Blending is authored by a real Graph composition pass.
    struct RuntimeOutputPlacement
    {
        std::int32_t layer{};
        std::optional<RuntimeTargetRect> rectangle;
    };

    struct RuntimeCapacity
    {
        std::uint32_t scenes{4}, views{8}, targets{8}, paths{4}, native_variants{8};
        std::uint32_t assets{16}, samplers{8}, queue_submissions{512}, frames_in_flight{2};
        std::uint32_t native_state_ranges{256};
    };

    struct RuntimeProgress
    {
        std::uint64_t render_frames{}, native_invocations{}, reused_scene_passes{};
        std::uint64_t publications{}, native_compilations{}, native_cache_hits{};
        std::uint64_t waits{}, deadline_misses{};
    };
} // namespace lux::render
