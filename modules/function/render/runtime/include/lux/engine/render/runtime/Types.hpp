#pragma once

#include <lux/engine/render/runtime/Objects.hpp>

namespace lux::render
{
    // The existing R2 endpoint identity scopes local SlotKeys across Server lifetimes.
    template <typename Local> struct RuntimeAddress
    {
        std::uint64_t owner{};
        Local local;
        bool operator==(const RuntimeAddress&) const noexcept = default;
    };

    using RuntimeScene = RuntimeAddress<RenderSceneId>;
    using RuntimeView = RuntimeAddress<ViewRef>;
    using RuntimeTarget = RuntimeAddress<RenderTargetId>;
    using RuntimeEffect = RuntimeAddress<RenderEffectId>;
    using RuntimeGraph = RuntimeAddress<RenderGraphId>;

    struct RuntimeCapacity
    {
        std::uint32_t scenes{4}, views{8}, targets{8}, effects{4}, graphs{8};
        std::uint32_t queue_submissions{512}, frames_in_flight{2};
    };

    struct RuntimeProgress
    {
        std::uint64_t render_frames{}, native_invocations{}, reused_scene_passes{};
        std::uint64_t publications{}, native_compilations{}, native_cache_hits{};
    };
} // namespace lux::render
