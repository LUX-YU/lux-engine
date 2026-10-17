#pragma once

#include <lux/engine/render/protocol/Protocol.hpp>
#include <stdexec/execution.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>

namespace native_test
{
    // Test/extension finite producer for a single real recorded submission. This is not a Runtime
    // service: the operation embeds the existing Queue callback and owns its actual Native candidate.
    template <class Response, class Work> class SubmissionSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(Response), stdexec::set_error_t(lux::render::RenderError),
            stdexec::set_error_t(lux::render::CommandFailure), stdexec::set_stopped_t()
        >;

        SubmissionSender(Response response, std::unique_ptr<Work> work) noexcept
            : response_(std::move(response)), work_(std::move(work)) {}

        explicit SubmissionSender(lux::render::RenderError error) noexcept
            : response_(lux::cxx::unexpected(error)) {}

        template <class Receiver> class Operation final
        {
        public:
            using operation_state_concept = stdexec::operation_state_t;

            Operation(Response response, std::unique_ptr<Work> work, Receiver receiver) noexcept
                : response_(std::move(response)), work_(std::move(work)), receiver_(std::move(receiver)) {}

            void start() noexcept
            {
                if (!work_) { stdexec::set_value(std::move(receiver_), std::move(response_)); return; }
                if (stdexec::get_stop_token(stdexec::get_env(receiver_)).stop_requested())
                {
                    work_.reset();
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                auto& queue = work_->queue();
                if (auto reserved = queue.reserve(callback_); !reserved)
                {
                    reject(reserved.error());
                    return;
                }
                const auto before = queue.submitted();
                auto submitted = work_->submit();
                if (!submitted)
                {
                    reject(submitted.error());
                    return;
                }
                // A previous or foreign successful Ticket is not evidence for this producer.
                const bool wrong_ticket = !queue.owns(*submitted) || submitted->serial() <= before;
                if (wrong_ticket)
                {
                    reject(lux::render::RenderError{lux::render::kProtocolContract});
                    return;
                }
                if (!queue.onComplete(*submitted, callback_, finish, this)) std::terminate();
            }

        private:
            void reject(lux::render::RenderError error) noexcept
            {
                callback_.reset();
                work_.reset();
                response_ = {};
                stdexec::set_error(std::move(receiver_), error);
            }

            static void finish(void* pointer, lux::render::RenderResult<void> result) noexcept
            {
                auto& self = *static_cast<Operation*>(pointer);
                const bool stopped = stdexec::get_stop_token(stdexec::get_env(self.receiver_)).stop_requested();
                const bool lost = self.work_->queue().deviceLost();
                if (result && !stopped && !lost) result = self.work_->ready();
                self.callback_.reset();
                self.work_.reset();
                auto receiver = std::move(self.receiver_);
                if (stopped || lost || !result) self.response_ = {};
                auto response = std::move(self.response_);
                if (stopped || lost)
                    stdexec::set_error(std::move(receiver), lux::render::CommandFailure{
                        lost ? lux::render::vulkan::nativeError(VK_ERROR_DEVICE_LOST) :
                            lux::render::RenderError{lux::render::vulkan::kWorkStopped}, true
                    });
                else if (!result)
                    stdexec::set_error(std::move(receiver), result.error());
                else
                    stdexec::set_value(std::move(receiver), std::move(response));
            }

            Response response_;
            std::unique_ptr<Work> work_;
            Receiver receiver_;
            lux::render::vulkan::SubmissionCallback callback_;
        };

        template <class Receiver> [[nodiscard]] Operation<std::decay_t<Receiver>> connect(Receiver&& receiver) &&
        {
            return {std::move(response_), std::move(work_), std::forward<Receiver>(receiver)};
        }

    private:
        Response response_;
        std::unique_ptr<Work> work_;
    };
}
