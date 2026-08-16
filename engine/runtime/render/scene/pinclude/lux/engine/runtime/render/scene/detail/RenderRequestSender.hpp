#pragma once
/**
 * @file RenderRequestSender.hpp
 * @brief Private stdexec adapter for one render-control RPC completion.
 *
 * The request continuation is installed only at this integration boundary.
 * Domain code keeps polling its owned RenderRequest at main-thread safe points;
 * this sender merely turns reply adoption into a close-progress edge.
 */

#include <lux/engine/function/render/client/RenderRequest.hpp>

#include <cassert>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

namespace lux::runtime::render_scene_detail
{
    template <class Reply>
    struct RenderRequestSender final
    {
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(Reply),
            stdexec::set_error_t(lux::render::RenderError)>;

        lux::render::RenderRequest<Reply> request;

        [[nodiscard]] stdexec::env<> get_env() const noexcept { return {}; }

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;

            lux::render::ScopedRenderRequest<Reply> request;
            Receiver receiver;

            void start() & noexcept
            {
                // This must remain the last operation: an already-adopted
                // reply completes synchronously and may destroy this state.
                request.then(
                    [this](const Reply& value) noexcept
                    {
                        if (request.failed())
                        {
                            stdexec::set_error(
                                std::move(receiver), request.error());
                            return;
                        }
                        stdexec::set_value(std::move(receiver), value);
                    });
            }
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) const
        {
            return {
                lux::render::ScopedRenderRequest<Reply>{
                    lux::render::RenderRequest<Reply>{request}},
                std::forward<Receiver>(receiver)};
        }
    };

    template <class Reply>
    [[nodiscard]] RenderRequestSender<Reply> asSender(
        lux::render::RenderRequest<Reply> request)
    {
        assert(request.valid());
        return {std::move(request)};
    }
} // namespace lux::runtime::render_scene_detail
