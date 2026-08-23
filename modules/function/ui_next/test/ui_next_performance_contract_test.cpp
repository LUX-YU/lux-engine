#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/ui_next/UiNext.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    class BenchmarkPane final : public lux::object::Object<BenchmarkPane, lux::ui::Pane>
    {
    public:
        BenchmarkPane(lux::object::ObjectDispatcherRef dispatcher, std::size_t index)
            : Object(
                  std::move(dispatcher),
                  lux::ui::PaneId{"pane." + std::to_string(index)},
                  lux::ui::PaneTypeId{"benchmark.pane"},
                  "Pane " + std::to_string(index)
              )
        {
        }

        [[nodiscard]] std::span<const lux::ui::UiContextIdView>
        contexts() const noexcept override
        {
            return contexts_;
        }

    protected:
        void draw(lux::ui::PaneDrawContext& context) override
        {
            context.activateContext(contexts_.front());
            ImGui::TextUnformatted("benchmark");
        }

    private:
        std::array<lux::ui::UiContextIdView, 1> contexts_{
            lux::ui::UiContextIdView{"benchmark.context"}
        };
    };

    class CommandReceiver final : public lux::object::Object<CommandReceiver>
    {
    public:
        void invoke() { ++invocations; }
        [[nodiscard]] bool enabled() const { return true; }
        std::size_t invocations{0};
    };

    void benchmarkPanes(std::size_t pane_count)
    {
        lux::ui::UISession session;
        std::vector<std::unique_ptr<BenchmarkPane>> panes;
        std::vector<lux::ui::UISession::Registration> registrations;
        panes.reserve(pane_count);
        registrations.reserve(pane_count);
        for (std::size_t index = 0; index < pane_count; ++index)
        {
            auto pane = std::make_unique<BenchmarkPane>(session.dispatcherRef(), index);
            auto registration = session.registerPane(*pane);
            assert(registration);
            registrations.push_back(std::move(*registration));
            panes.push_back(std::move(pane));
        }

        for (int frame = 0; frame < 3; ++frame)
        {
            session.beginFrame({1280.0F, 720.0F}, 1.0F / 60.0F);
            session.drawPanes();
            static_cast<void>(session.endFrame());
        }
        const auto start = Clock::now();
        for (int frame = 0; frame < 10; ++frame)
        {
            session.beginFrame({1280.0F, 720.0F}, 1.0F / 60.0F);
            session.drawPanes();
            static_cast<void>(session.endFrame());
        }
        const auto elapsed = Clock::now() - start;
        std::cout
            << "ui_panes," << pane_count << ','
            << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
            << '\n';
    }

    void benchmarkCommands(
        std::size_t binding_count,
        std::size_t context_count,
        std::size_t query_count
    )
    {
        lux::ui::CommandRouter router;
        CommandReceiver receiver;
        std::vector<lux::ui::CommandRegistration> registrations;
        registrations.reserve(binding_count);
        for (std::size_t index = 0; index < binding_count; ++index)
        {
            auto command = router.defineCommand(
                {lux::ui::UiCommandId{"command." + std::to_string(index)}, "Command"}
            );
            assert(command);
            auto registration =
                router.bindGlobal<&CommandReceiver::invoke, &CommandReceiver::enabled>(
                    *command,
                    receiver
                );
            assert(registration);
            registrations.push_back(std::move(*registration));
        }

        constexpr std::array specific_contexts{
            lux::ui::UiContextIdView{"context.0"},
            lux::ui::UiContextIdView{"context.1"},
            lux::ui::UiContextIdView{"context.2"},
            lux::ui::UiContextIdView{"context.3"},
            lux::ui::UiContextIdView{"context.4"},
            lux::ui::UiContextIdView{"context.5"},
            lux::ui::UiContextIdView{"context.6"}
        };
        std::array<lux::ui::UiContextIdView, 8> active_contexts{};
        for (std::size_t index = 0; index + 1 < context_count; ++index)
            active_contexts[index] = specific_contexts[index];
        active_contexts[context_count - 1] = lux::ui::kGlobalContext;
        router.setActiveContexts(std::span{active_contexts}.first(context_count));

        const auto start = Clock::now();
        for (std::size_t index = 0; index < query_count; ++index)
        {
            const auto command =
                lux::ui::CommandIndex{static_cast<std::uint32_t>(index % binding_count)
                };
            assert(router.state(command).enabled);
        }
        const auto elapsed = Clock::now() - start;
        std::cout
            << "ui_commands," << binding_count << ',' << context_count << ','
            << query_count << ','
            << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
            << '\n';
    }
} // namespace

int main()
{
    std::cout << "case,count,contexts_or_elapsed,queries_or_empty,elapsed_ns\n";
    for (const auto pane_count : {10U, 50U, 200U})
        benchmarkPanes(pane_count);
    for (const auto binding_count : {100U, 500U})
    {
        for (const auto context_count : {1U, 3U, 8U})
        {
            benchmarkCommands(binding_count, context_count, 50);
            benchmarkCommands(binding_count, context_count, 200);
        }
    }
}
