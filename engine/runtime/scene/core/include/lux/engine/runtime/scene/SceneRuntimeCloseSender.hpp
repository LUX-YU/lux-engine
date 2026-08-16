#pragma once
/**
 * @file SceneRuntimeCloseSender.hpp
 * @brief Opt-in sender adapter for SceneRuntime shutdown.
 */

#include <lux/engine/runtime/scene/SceneRuntime.hpp>

#include <stdexec/execution.hpp>

#include <type_traits>
#include <utility>

namespace lux::runtime
{
    class SceneRuntimeCloseSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(SceneCloseReport)>;

        SceneRuntimeCloseSender() noexcept = default;

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            SceneRuntime* runtime{nullptr};
            Receiver receiver;

            void start() & noexcept
            {
                if (runtime == nullptr)
                {
                    stdexec::set_value(
                        std::move(receiver), SceneCloseReport{});
                    return;
                }
                runtime->subscribeClose(
                    [this](SceneCloseReport report) mutable noexcept
                    {
                        stdexec::set_value(
                            std::move(receiver), std::move(report));
                    });
            }
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            return {
                std::exchange(runtime_, nullptr),
                std::forward<Receiver>(receiver)};
        }

    private:
        friend class SceneRuntime;
        explicit SceneRuntimeCloseSender(SceneRuntime& runtime) noexcept
            : runtime_(&runtime)
        {}

        SceneRuntime* runtime_{nullptr};
    };
}
