#pragma once

#include <lux/cxx/container/SlotMap.hpp>

namespace lux::render
{
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
