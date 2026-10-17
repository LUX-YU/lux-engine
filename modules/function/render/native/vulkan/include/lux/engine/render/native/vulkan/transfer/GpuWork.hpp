#pragma once

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/Transfer.hpp>
#include <lux/engine/render/protocol/AsyncCommand.hpp>
#include <optional>
#include <utility>

namespace lux::render::vulkan
{
    // One Native Transfer and its professional publication candidate. There is no erased GPU driver:
    // all pending work uses the same concrete Transfer state machine and its real Queue callbacks.
    class GpuWork final
    {
    public:
        GpuWork() noexcept = default;
        ~GpuWork() noexcept { reset(); }
        GpuWork(GpuWork&&) noexcept = default;
        GpuWork& operator=(GpuWork&& other) noexcept
        {
            if (this == &other) return *this;
            reset();
            session_ = other.session_;
            transfer_ = std::move(other.transfer_);
            owner_ = std::move(other.owner_);
            ready_ = std::exchange(other.ready_, nullptr);
            accepted_ready_ = std::exchange(other.accepted_ready_, nullptr);
            accepted_ = std::exchange(other.accepted_, std::nullopt);
            return *this;
        }
        GpuWork(const GpuWork&) = delete;
        GpuWork& operator=(const GpuWork&) = delete;

        template <auto Ready, class T>
        [[nodiscard]] static RenderResult<GpuWork> pending(
            DeviceSession& session, std::unique_ptr<Transfer>& transfer, std::unique_ptr<T>& candidate
        ) noexcept
        {
            if (!transfer || !candidate || !transfer->unstartedIn(session))
                return cxx::unexpected(RenderError{kWrongOwner});
            GpuWork result;
            result.session_ = &session;
            result.transfer_ = std::move(transfer);
            result.owner_ = {candidate.release(), [](void* value) noexcept { delete static_cast<T*>(value); }};
            result.ready_ = [](void* value, Transfer& work, DeviceSession& device) noexcept {
                return Ready(*static_cast<T*>(value), work, device);
            };
            return result;
        }

        [[nodiscard]] static GpuWork accepted(DeviceSession& session, SubmissionTicket ticket,
            void* owner, void (*destroy)(void*) noexcept,
            RenderResult<void> (*ready)(void*, DeviceSession&) noexcept) noexcept;
        [[nodiscard]] bool unstartedIn(const DeviceSession& session) const noexcept
        {
            return owner_ && transfer_ && session_ == &session && transfer_->unstartedIn(session);
        }
        [[nodiscard]] bool submitted() const noexcept
        { return transfer_ ? !transfer_->requirements().empty() : accepted_.has_value(); }
        [[nodiscard]] bool deviceLost() const noexcept { return session_->deviceLost(); }
        [[nodiscard]] RenderResult<void> prepareCompletion() noexcept;
        void start(CompletionPost post, void* owner, Transfer::TerminalFn terminal,
            std::shared_ptr<const void> implementation, std::shared_ptr<const void> registration) noexcept;
        [[nodiscard]] RenderResult<void> publish() noexcept;
        void cancel() noexcept { if (transfer_) transfer_->cancel(); }
        void reset() noexcept
        {
            // Candidate rollback destroys dependent views/samplers before Transfer releases its image.
            // Transfer teardown borrows only DeviceSession/pages/Permits, never the candidate Storage.
            owner_.reset();
            transfer_.reset();
            ready_ = nullptr;
            accepted_ready_ = nullptr;
            accepted_.reset();
        }

    private:
        DeviceSession* session_ = nullptr;
        std::unique_ptr<Transfer> transfer_;
        std::unique_ptr<void, void (*)(void*) noexcept> owner_{nullptr, [](void*) noexcept {}};
        RenderResult<void> (*ready_)(void*, Transfer&, DeviceSession&) noexcept = nullptr;
        RenderResult<void> (*accepted_ready_)(void*, DeviceSession&) noexcept = nullptr;
        std::optional<SubmissionTicket> accepted_;
    };
    // Finite Native adapter around the existing Transfer. It owns no request, queue or Device.
    // The caller reserves Reply capacity and reborrows its stable input before creating this work.
    template <class Response> class GpuWorkSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(Response), stdexec::set_error_t(RenderError),
            stdexec::set_error_t(CommandFailure), stdexec::set_stopped_t()
        >;

        GpuWorkSender(Response response, GpuWork work) noexcept
            : response_(std::move(response)), work_(std::move(work)) {}

        template <class Receiver> class Operation final
        {
            struct OnStop final
            {
                Operation* operation;
                void operator()() const noexcept { operation->work_.cancel(); }
            };

        public:
            using operation_state_concept = stdexec::operation_state_t;
            Operation(Response response, GpuWork work, Receiver receiver) noexcept
                : response_(std::move(response)), work_(std::move(work)), receiver_(std::move(receiver)) {}
            Operation(const Operation&) = delete;
            Operation(Operation&&) = delete;

            void start() noexcept
            {
                const auto environment = stdexec::get_env(receiver_);
                const auto token = stdexec::get_stop_token(environment);
                if (token.stop_requested())
                {
                    work_.reset();
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                if (auto prepared = work_.prepareCompletion(); !prepared)
                {
                    const auto error = prepared.error();
                    work_.reset();
                    stdexec::set_error(std::move(receiver_), error);
                    return;
                }
                stop_.emplace(token, OnStop{this});
                auto pins = execution::getWorkCodePins(environment);
                work_.start(execution::getCompletionPost(environment), this, finish,
                    std::move(pins.first), std::move(pins.second));
                // start/finish may synchronously publish; no state access follows publication.
            }

        private:
            static void finish(void* value, RenderResult<void> result) noexcept
            {
                auto& self = *static_cast<Operation*>(value);
                const bool stopping = stdexec::get_stop_token(stdexec::get_env(self.receiver_)).stop_requested();
                const bool device_lost = self.work_.deviceLost();
                const bool unknown = (stopping || device_lost) && self.work_.submitted();
                if (result && !stopping && !device_lost) result = self.work_.publish();
                self.work_.reset(); // Native candidate ends before the source Request can be returned.
                auto receiver = std::move(self.receiver_);
                auto response = std::move(self.response_);
                if (unknown)
                    stdexec::set_error(std::move(receiver), CommandFailure{
                        device_lost ? nativeError(VK_ERROR_DEVICE_LOST) : RenderError{kWorkStopped}, true
                    });
                else if (stopping)
                    stdexec::set_stopped(std::move(receiver));
                else if (!result)
                    stdexec::set_error(std::move(receiver), result.error());
                else
                    stdexec::set_value(std::move(receiver), std::move(response));
            }

            Response response_;
            GpuWork work_;
            Receiver receiver_;
            std::optional<stdexec::inplace_stop_callback<OnStop>> stop_;
        };

        template <class Receiver> [[nodiscard]] Operation<std::decay_t<Receiver>> connect(Receiver&& receiver) &&
        {
            return {std::move(response_), std::move(work_), std::forward<Receiver>(receiver)};
        }

    private:
        Response response_;
        GpuWork work_;
    };

}
