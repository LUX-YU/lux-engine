#pragma once

#include <atomic>
#include <lux/engine/object/ObjectTarget.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <optional>

namespace lux::process
{
    namespace detail
    {
        class ObjectScheduleSender;
    }

    // Only set_value is delivered under a live Object callback borrow on the owner thread.
    // Rejection/pre-start stop complete on the caller; accepted cancellation waits for queue delivery.
    class ObjectScheduler final
    {
    public:
        ObjectScheduler() noexcept = default;
        [[nodiscard]] detail::ObjectScheduleSender schedule() const noexcept;
        friend bool operator==(const ObjectScheduler&, const ObjectScheduler&) noexcept = default;

    private:
        friend ObjectScheduler objectScheduler(object::LuxObject&) noexcept;
        friend class detail::ObjectScheduleSender;
        explicit ObjectScheduler(object::ObjectTarget target) noexcept;
        object::ObjectTarget target_;
    };
    [[nodiscard]] ObjectScheduler objectScheduler(object::LuxObject&) noexcept;

    namespace detail
    {
        class ObjectScheduleSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(),
                stdexec::set_error_t(EExecutionError),
                stdexec::set_stopped_t()>;

            class Env final
            {
            public:
                explicit Env(ObjectScheduler scheduler) noexcept : scheduler_(std::move(scheduler)) {}

                [[nodiscard]] ObjectScheduler query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>)
                    const noexcept
                {
                    return scheduler_;
                }

            private:
                ObjectScheduler scheduler_;
            };
            [[nodiscard]] Env get_env() const noexcept
            {
                return Env{scheduler_};
            }

            template <class Receiver> class TOperation final
            {
            public:
                using operation_state_concept = stdexec::operation_state_t;
                using StopToken = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;
                struct Cancel final
                {
                    std::atomic_bool* requested{};
                    void operator()() noexcept
                    {
                        requested->store(true, std::memory_order_release);
                    }
                };
                using StopCallback = stdexec::stop_callback_for_t<StopToken, Cancel>;

                TOperation(object::ObjectTarget target, Receiver receiver)
                    : target_(std::move(target)), receiver_(std::move(receiver))
                {
                }

                TOperation(const TOperation&) = delete;
                TOperation& operator=(const TOperation&) = delete;
                TOperation(TOperation&&) = delete;
                TOperation& operator=(TOperation&&) = delete;

                void start() & noexcept
                {
                    auto token = stdexec::get_stop_token(stdexec::get_env(receiver_));
                    if (token.stop_requested())
                    {
                        stdexec::set_stopped(std::move(receiver_));
                        return;
                    }
                    stop_callback_.emplace(token, Cancel{&cancel_requested_});
                    const auto status = object::post(
                        target_,
                        [this](object::LuxObject* object) noexcept
                        {
                            // Unregister cancellation before completing: receiver may destroy this operation.
                            stop_callback_.reset();
                            const bool stopped = !object || cancel_requested_.load(std::memory_order_acquire);
                            if (stopped)
                            {
                                stdexec::set_stopped(std::move(receiver_));
                            }
                            else
                            {
                                stdexec::set_value(std::move(receiver_));
                            }
                        }
                    );
                    // POSTED may have completed concurrently. Never touch this again on that path.
                    if (status == object::EObjectPostStatus::POSTED)
                    {
                        return;
                    }
                    stop_callback_.reset();
                    if (status == object::EObjectPostStatus::FULL)
                    {
                        stdexec::set_error(std::move(receiver_), EExecutionError::CAPACITY_EXCEEDED);
                    }
                    else
                    {
                        stdexec::set_stopped(std::move(receiver_));
                    }
                }

            private:
                object::ObjectTarget target_;
                Receiver receiver_;
                std::atomic_bool cancel_requested_{};
                std::optional<StopCallback> stop_callback_;
            };
            template <class Receiver>
            [[nodiscard]] TOperation<std::decay_t<Receiver>> connect(Receiver&& receiver) const
            {
                return TOperation<std::decay_t<Receiver>>{scheduler_.target_, std::forward<Receiver>(receiver)};
            }

        private:
            friend class ::lux::process::ObjectScheduler;
            explicit ObjectScheduleSender(ObjectScheduler scheduler) noexcept : scheduler_(std::move(scheduler)) {}

            ObjectScheduler scheduler_;
        };
    } // namespace detail

    inline detail::ObjectScheduleSender ObjectScheduler::schedule() const noexcept
    {
        return detail::ObjectScheduleSender{*this};
    }

    static_assert(stdexec::scheduler<ObjectScheduler>);
} // namespace lux::process
