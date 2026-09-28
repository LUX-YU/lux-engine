#pragma once

#include <lux/engine/editor/editing/EditTypes.hpp>
#include <lux/engine/editor/sessions/SessionId.hpp>

namespace lux::editor::sessions
{
    struct ContentStamp final
    {
        SessionId session;
        editing::StateId state;
        friend bool operator==(ContentStamp, ContentStamp) noexcept = default;
    };
    struct ObservationVersion final
    {
        std::uint64_t value{};
        friend bool operator==(ObservationVersion, ObservationVersion) noexcept = default;
    };
    struct BindingRevision final
    {
        std::uint64_t value{1};
        friend bool operator==(BindingRevision, BindingRevision) noexcept = default;
    };
    struct PublicationOrder final
    {
        std::uint64_t value{};
        friend auto operator<=>(PublicationOrder, PublicationOrder) noexcept = default;
    };
}
