#pragma once

#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>

// Test callers deliberately retain or copy their own input when exercising repeated submissions.
// Production submit remains move-on-success and preserves the exact input on admission failure.
template <lux::render::RenderCommand C>
auto submitValue(lux::render::RenderClient& client, const lux::render::BoundCommand<C>& binding,
    typename C::Request input, std::size_t reply_capacity = 128) noexcept
{
    return client.submit(binding, input, reply_capacity);
}

template <class T, lux::render::RenderCommand C>
T decoded(lux::render::TypedRenderCompletion<C> completion)
{
    static_assert(std::is_same_v<T, typename C::Response>);
    static_assert(std::is_trivially_copyable_v<T>);
    return completion.response();
}
