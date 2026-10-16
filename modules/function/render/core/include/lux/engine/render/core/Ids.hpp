#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <limits>

namespace lux::render
{
    inline constexpr auto kGraphInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    // Owner-local indices. Equal numbers in different definitions are unrelated.
    struct GraphPassId final
    {
        std::uint32_t index = kGraphInvalidIndex;
        bool operator==(const GraphPassId&) const noexcept = default;
    };

    struct GraphResourceId final
    {
        std::uint32_t index = kGraphInvalidIndex;
        bool operator==(const GraphResourceId&) const noexcept = default;
    };

    struct SceneTag;
    struct ViewTag;
    struct TargetTag;
    struct EffectTag;

    using SceneId   = cxx::SlotKey<SceneTag>;
    using ViewId    = cxx::SlotKey<ViewTag>;
    using TargetId  = cxx::SlotKey<TargetTag>;
    using EffectId  = cxx::SlotKey<EffectTag>;

    // Keys are owner-local. isValid() only checks the null sentinel; lookup must
    // validate generation in the owning SlotMap. View lookup also needs SceneId.
    // Keys must not cross Server/Endpoint lifetimes or be persisted as global IDs.
}
