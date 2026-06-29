#pragma once
// ============================================================================
//  ResourceOperationCommon.hpp — shapes shared across resource op payloads
// ============================================================================

#include <type_traits>

namespace lux::render
{
    /// Wire payload for "destroy the resource named by `handle`". Every
    /// resource kind's Destroy*Payload is an alias of this template, so they
    /// share one definition while remaining distinct types (each keeps its own
    /// type_id for command dispatch). Trivially copyable whenever HandleT is.
    template <class HandleT>
    struct DestroyResourcePayload
    {
        HandleT handle{};
    };
} // namespace lux::render
