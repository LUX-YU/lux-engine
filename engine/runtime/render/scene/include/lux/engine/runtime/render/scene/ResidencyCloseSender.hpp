#pragma once
/**
 * @file ResidencyCloseSender.hpp
 * @brief Opt-in sender adapter for process-domain residency shutdown.
 */

#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>

#include <stdexec/execution.hpp>

#include <type_traits>
#include <utility>

namespace lux::runtime
{
    class ResidencyCloseSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(ResidencyCloseReport)>;

        ResidencyCloseSender() noexcept = default;

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            ResidencyAssembly* assembly{nullptr};
            Receiver receiver;

            void start() & noexcept
            {
                if (assembly == nullptr)
                {
                    stdexec::set_value(
                        std::move(receiver), ResidencyCloseReport{});
                    return;
                }
                assembly->subscribeClose(
                    [this](ResidencyCloseReport report) mutable noexcept
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
                std::exchange(assembly_, nullptr),
                std::forward<Receiver>(receiver)};
        }

    private:
        friend class ResidencyAssembly;
        explicit ResidencyCloseSender(ResidencyAssembly& assembly) noexcept
            : assembly_(&assembly)
        {}

        ResidencyAssembly* assembly_{nullptr};
    };
}
