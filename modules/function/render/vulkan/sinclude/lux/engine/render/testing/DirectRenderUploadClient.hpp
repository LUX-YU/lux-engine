#pragma once

// Project-visible test adapter: a low-level render test makes its driving
// thread the upload coordinator while still using the owning
// RenderUploadClient packet protocol. Product composition roots bind through
// AsyncRenderUploadService instead.

#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>

#include <memory>
#include <utility>

namespace lux::render::testing
{
    class DirectRenderUploadClient final
    {
    public:
        explicit DirectRenderUploadClient(RenderUploadSession& session) : state_(std::make_shared<State>())
        {
            state_->session = &session;
        }

        ~DirectRenderUploadClient()
        {
            state_->session = nullptr;
        }

        DirectRenderUploadClient(const DirectRenderUploadClient&) = delete;
        DirectRenderUploadClient& operator=(const DirectRenderUploadClient&) = delete;
        DirectRenderUploadClient(DirectRenderUploadClient&&) = delete;
        DirectRenderUploadClient& operator=(DirectRenderUploadClient&&) = delete;

        [[nodiscard]] RenderUploadClient client() const noexcept
        {
            return RenderUploadClient::bind(state_, &DirectRenderUploadClient::submit);
        }

    private:
        struct State final
        {
            RenderUploadSession* session{nullptr};
        };

        [[nodiscard]] static UploadSubmitNoReplyResult
        submit(void* opaque, std::shared_ptr<detail::PreparedUpload> prepared) noexcept
        {
            auto* state = static_cast<State*>(opaque);
            if (!state || !state->session || !prepared)
            {
                return lux::cxx::unexpected(ERenderUploadSubmitError::STOPPING);
            }
            if (prepared->expected_reply_type == kInvalidTypeId)
            {
                return state->session->trySubmitPreparedNoReply(prepared->packet);
            }
            return state->session->trySubmitPrepared(
                prepared->packet,
                prepared->expected_reply_type,
                std::move(prepared->callback)
            );
        }

        std::shared_ptr<State> state_;
    };
}
