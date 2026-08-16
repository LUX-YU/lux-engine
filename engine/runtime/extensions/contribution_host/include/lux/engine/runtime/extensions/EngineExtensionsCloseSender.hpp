#pragma once

#include <lux/engine/runtime/extensions/EngineExtensions.hpp>

#include <stdexec/execution.hpp>

#include <type_traits>
#include <utility>

namespace lux::extensions
{
    class EngineExtensionsCloseSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(EngineExtensionsCloseReport)>;

        EngineExtensionsCloseSender() noexcept = default;

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            EngineExtensions* owner{nullptr};
            Receiver receiver;

            void start() & noexcept
            {
                if (owner == nullptr)
                {
                    stdexec::set_value(
                        std::move(receiver),
                        EngineExtensionsCloseReport{});
                    return;
                }
                subscribeEngineExtensionsClose(
                    *owner,
                    [this](EngineExtensionsCloseReport report) mutable noexcept
                    {
                        stdexec::set_value(
                            std::move(receiver),
                            std::move(report));
                    });
            }
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            return {
                std::exchange(owner_, nullptr),
                std::forward<Receiver>(receiver)};
        }

    private:
        friend class EngineExtensions;
        explicit EngineExtensionsCloseSender(
            EngineExtensions& owner) noexcept
            : owner_(&owner)
        {}

        EngineExtensions* owner_{nullptr};
    };
}
