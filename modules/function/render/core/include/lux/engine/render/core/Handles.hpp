#pragma once

#include <lux/cxx/container/SlotMap.hpp>

namespace lux::render
{
    struct RenderSceneTag;
    struct RenderViewTag;
    struct RenderTargetTag;
    struct FeatureInstanceTag;

    // Non-owning, allocator-local tokens. isValid() checks only the null sentinel;
    // the owning registry must check generation and runtime/domain membership.
    // Never persist these tokens or use them across their owner's lifetime.
    using RenderSceneId = cxx::SlotKey<RenderSceneTag>;
    using RenderViewHandle = cxx::SlotKey<RenderViewTag>;
    using RenderTargetId = cxx::SlotKey<RenderTargetTag>;
    using FeatureHandle = cxx::SlotKey<FeatureInstanceTag>;

    template <typename ResourceTag>
    using RenderResourceHandle = cxx::SlotKey<ResourceTag>;
}
