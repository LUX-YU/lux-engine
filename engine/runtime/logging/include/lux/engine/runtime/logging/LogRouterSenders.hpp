#pragma once

#include <lux/engine/runtime/logging/LogRouter.hpp>

#include <stdexec/execution.hpp>

#include <type_traits>
#include <utility>

namespace lux::logging
{
    class LogRouterCloseSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(LogRouterStatistics)>;

        LogRouterCloseSender() noexcept = default;

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            std::shared_ptr<void> state;
            void (*subscribe)(
                std::shared_ptr<void>,
                lux::cxx::move_only_function<void(LogRouterStatistics)>)
                noexcept{nullptr};
            Receiver receiver;

            void start() & noexcept
            {
                subscribe(
                    std::move(state),
                    [this](LogRouterStatistics statistics) mutable noexcept
                    {
                        stdexec::set_value(
                            std::move(receiver), std::move(statistics));
                    });
            }
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            return {std::move(state_), subscribe_,
                    std::forward<Receiver>(receiver)};
        }

    private:
        friend class LogRouter;
        LogRouterCloseSender(
            std::shared_ptr<void> state,
            void (*subscribe)(
                std::shared_ptr<void>,
                lux::cxx::move_only_function<void(LogRouterStatistics)>)
                noexcept) noexcept
            : state_(std::move(state)), subscribe_(subscribe)
        {}

        std::shared_ptr<void> state_;
        void (*subscribe_)(
            std::shared_ptr<void>,
            lux::cxx::move_only_function<void(LogRouterStatistics)>)
            noexcept{nullptr};
    };
}
