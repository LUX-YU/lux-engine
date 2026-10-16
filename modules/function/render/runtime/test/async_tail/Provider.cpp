#include "../ErasedRegistrationFixture.hpp"
#include "Fixture.hpp"
#include <lux/engine/render/protocol/AsyncCommand.hpp>

namespace async_tail_test
{
    using namespace lux::render;

    // This provider's terminal callback deliberately remains on its DLL stack
    // after it has allowed the Runtime to destroy the entire operation state.
    class TailSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(std::uint32_t),
            stdexec::set_error_t(RenderError),
            stdexec::set_stopped_t()
        >;

        TailSender(CpuSender source, Gate& gate, std::uint32_t value) noexcept
            : source_(std::move(source)), gate_(&gate), value_(value)
        {}

        template <class Receiver> class Operation final
        {
            struct Scheduled final
            {
                using receiver_concept = stdexec::receiver_t;
                Operation* operation;

                [[nodiscard]] stdexec::env_of_t<Receiver> get_env() const noexcept
                {
                    return stdexec::get_env(operation->receiver_);
                }

                void deliver(ETerminal terminal, RenderError error = {}) && noexcept
                {
                    // Snapshot everything before terminal delivery. The parent
                    // operation can be destroyed by that delivery on another thread.
                    auto* gate = operation->gate_;
                    const auto value = operation->value_;
                    auto receiver = std::move(operation->receiver_);
                    switch (terminal)
                    {
                    case ETerminal::VALUE: stdexec::set_value(std::move(receiver), value); break;
                    case ETerminal::FAILURE: stdexec::set_error(std::move(receiver), error); break;
                    case ETerminal::STOPPED: stdexec::set_stopped(std::move(receiver)); break;
                    }
                    gate->published.release();
                    gate->release.acquire();
                    gate->tails_returned.fetch_add(1, std::memory_order_release);
                    // Local receiver destruction and this return still execute in
                    // this DLL. Only the actual host trampoline can protect them.
                }

                void set_value() && noexcept
                {
                    const auto terminal = operation->gate_->terminal;
                    std::move(*this).deliver(terminal, RenderError{kProtocolContract, {73}});
                }

                void set_error(RenderError error) && noexcept
                {
                    std::move(*this).deliver(ETerminal::FAILURE, error);
                }

                void set_stopped() && noexcept
                {
                    std::move(*this).deliver(ETerminal::STOPPED);
                }
            };

            using SourceOperation = stdexec::connect_result_t<CpuSender, Scheduled>;

        public:
            using operation_state_concept = stdexec::operation_state_t;

            Operation(TailSender source, Receiver receiver)
                : receiver_(std::move(receiver)), gate_(source.gate_), value_(source.value_),
                  source_(stdexec::connect(std::move(source.source_), Scheduled{this}))
            {}

            ~Operation() noexcept
            {
                gate_->states_destroyed.fetch_add(1, std::memory_order_release);
            }

            Operation(const Operation&) = delete;
            Operation& operator=(const Operation&) = delete;
            Operation(Operation&&) = delete;
            Operation& operator=(Operation&&) = delete;

            void start() & noexcept { stdexec::start(source_); }

        private:
            Receiver receiver_;
            Gate* gate_;
            std::uint32_t value_;
            SourceOperation source_;
        };

        template <class Receiver>
        [[nodiscard]] Operation<std::decay_t<Receiver>> connect(Receiver&& receiver) &&
        {
            return {std::move(*this), std::forward<Receiver>(receiver)};
        }

    private:
        CpuSender source_;
        Gate* gate_;
        std::uint32_t value_;
    };

    static RenderResult<void> invoke(Host& host, const Command::Request& value, CommandContext<Command>& context) noexcept
    {
        auto pin = host.module.lock();
        if (!pin) return lux::cxx::unexpected(RenderError{kProtocolContract});
        // CpuSchedule::bind was instantiated in the EXE. Its real worker
        // trampoline retains both code pins across this provider's callback.
        auto source = host.cpu.schedule([](void*) noexcept {}, nullptr, pin, pin);
        return context.completeWhen(TailSender{std::move(source), *host.gate, value});
    }
}

extern "C" __declspec(dllexport) void makeAsyncTailFeature(
    async_tail_test::Host& host, const std::shared_ptr<const void>& pin, lux::render::FeatureRegistration& output
) noexcept
{
    using namespace lux::render;
    static const std::array commands{commandDescriptor<async_tail_test::Command>()};
    static const FeatureDescriptor definition{FeatureName("test.async.dll_callback_tail.feature"), {1, 0}, nullptr};
    static const FeatureGraphOps definition_graph{[](const RetainableEncodedDataView*, std::string_view, std::vector<ResourceClaim>&) noexcept -> RenderResult<void> {
            return {};
        }, nullptr};
    host.handlers[0] = bindCommand<async_tail_test::Command, async_tail_test::invoke>(host);
    output = {&definition, &definition_graph, nullptr, host.handlers, pin};
}
