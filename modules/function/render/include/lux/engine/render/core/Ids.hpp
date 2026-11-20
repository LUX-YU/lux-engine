#pragma once

#include <cstdint>
#include <limits>
#include <lux/cxx/container/SlotMap.hpp>

namespace lux::render
{
    inline constexpr auto kGraphInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    // Definition-local indices. Equal numbers in different definitions are unrelated.
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

    struct RenderJobTag;
    struct DeviceSessionTag;
    struct SceneTag;
    struct ViewTag;
    struct TargetTag;
    struct EffectTag;
    struct ResourceTypeTag;
    struct ResourceInstanceTag;
    struct CommandTag;

    using RenderJobId = cxx::SlotKey<RenderJobTag>;
    using DeviceSessionId = cxx::SlotKey<DeviceSessionTag>;
    using SceneId = cxx::SlotKey<SceneTag>;
    using ViewId = cxx::SlotKey<ViewTag>;
    using TargetId = cxx::SlotKey<TargetTag>;
    using EffectId = cxx::SlotKey<EffectTag>;
    using ResourceTypeId = cxx::SlotKey<ResourceTypeTag>;
    using ResourceInstanceId = cxx::SlotKey<ResourceInstanceTag>;
    using CommandId = cxx::SlotKey<CommandTag>;

    // Keys are local to their SlotMap. isValid() only checks the null sentinel; lookup must
    // validate generation in the owning SlotMap. View lookup also needs SceneId.
    // Keys must not cross Server lifetimes or be persisted as global IDs.
} // namespace lux::render
