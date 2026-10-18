#include "../FeatureFixture.hpp"
#include "Fixture.hpp"
#include <lux/engine/render/runtime/CommandBinding.hpp>

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
            stdexec::set_value_t(Command::Response),
            stdexec::set_error_t(RenderError),
            stdexec::set_stopped_t()
        >;

        using CpuSource = decltype(stdexec::schedule(lux::execution::CpuScheduler{}));

        TailSender(CpuSource source, Gate& gate, std::uint32_t value) noexcept
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
                    case ETerminal::VALUE: stdexec::set_value(std::move(receiver), Command::Response{value}); break;
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

                void set_error(lux::execution::EExecutionError error) && noexcept
                {
                    std::move(*this).deliver(ETerminal::FAILURE, RenderError{kCpuScheduleFailure, {static_cast<std::uint64_t>(error)}});
                }

                void set_stopped() && noexcept
                {
                    std::move(*this).deliver(ETerminal::STOPPED);
                }
            };

            using SourceOperation = stdexec::connect_result_t<CpuSource, Scheduled>;

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
        CpuSource source_;
        Gate* gate_;
        std::uint32_t value_;
    };

    static TailSender invoke(Host& host, Command::Request&& value) noexcept
    {
        // The Host joins its worker before unloading this callback code.
        auto source = stdexec::schedule(host.cpu);
        return TailSender{std::move(source), *host.gate, value};
    }
}

namespace async_tail_test
{
    struct TailCommands final
    {
        static constexpr std::string_view name = "test.async.tail.commands";
        Host& host;
        explicit TailCommands(Host& value) noexcept : host(value) {}
        TailSender run(Command::Request&& input) noexcept
        { return invoke(host, std::move(input)); }
        using Bindings = std::tuple<lux::render::CommandMethod<Command, &TailCommands::run>>;
    };
}

extern "C" __declspec(dllexport) void makeAsyncTailFeature(
    lux::render::RenderServer& server, async_tail_test::Host& host,
    lux::render::RenderResult<lux::render::FeatureId>& output
) noexcept
{
    using namespace lux::render;
    static const FeatureDescriptor definition{FeatureName("test.async.dll_callback_tail.feature"), {}};
    static const FeatureGraphOps graph{[](const void*, std::string_view,
        ResourceClaims&) noexcept -> RenderResult<void> { return {}; }, nullptr};
    output = server.registerFeature<async_tail_test::Command>(
        feature_test::Provider<async_tail_test::TailCommands>{{&definition, &graph, nullptr}}, "", host);
}
