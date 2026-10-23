#pragma once

#include <exec/variant_sender.hpp>
#include <lux/engine/rendering/RenderRequester.hpp>

namespace lux::editor::detail
{
    // Cold lifecycle request: the Sender owns the request through its response. Frame updates use bound IDs.
    template <render::RenderCommand C>
    auto requestRenderCommand(rendering::RenderRequester& client, typename C::Request input) noexcept
    {
        return stdexec::let_value(
            stdexec::just(std::move(input), client.bind<C>()),
            [&client](typename C::Request& input, auto& binding) noexcept
            {
                using Immediate = decltype(stdexec::just(typename C::Response{}));
                using Request = decltype(client.request(*binding, input));
                using VRequest = exec::variant_sender<Immediate, Request>;
                return binding ? VRequest{client.request(*binding, input)}
                               : VRequest{stdexec::just(typename C::Response{binding.error()})};
            }
        );
    }
} // namespace lux::editor::detail
