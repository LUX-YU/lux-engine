#include <cstdlib>

#include <lux/engine/ui/UI.hpp>
#include <lux/engine/ui/detail/CommandRouterDiagnostics.hpp>
#include <lux/engine/ui/detail/DragDropEncoding.hpp>
#include <lux/engine/ui/detail/UISessionDiagnostics.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace
{
#define LUX_CHECK(condition)                                                                       \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
            std::abort();                                                                          \
    } while (false)

    using Clock = std::chrono::steady_clock;
    constexpr std::size_t kWarmupCount = 5;
    constexpr std::size_t kSampleCount = 30;
    std::atomic_size_t allocations{0};

    struct Sample final
    {
        std::uint64_t elapsed_ns{0};
        std::size_t allocations{0};
    };

    template <class Callable>
    void benchmark(std::string_view name, std::size_t count, std::size_t contexts,
                   Callable &&callable)
    {
        for (std::size_t index = 0; index < kWarmupCount; ++index)
            callable();

        std::vector<Sample> samples;
        samples.reserve(kSampleCount);
        for (std::size_t index = 0; index < kSampleCount; ++index)
        {
            const auto allocations_before = allocations.load(std::memory_order_relaxed);
            const auto begin = Clock::now();
            callable();
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
            const auto allocation_delta =
                allocations.load(std::memory_order_relaxed) - allocations_before;
            samples.push_back({elapsed, allocation_delta});
            std::cout << "raw," << name << ',' << count << ',' << contexts << ',' << index << ','
                      << elapsed << ',' << allocation_delta << '\n';
        }

        std::ranges::sort(samples, {}, [](const Sample &value) { return value.elapsed_ns; });
        const auto median =
            (samples[kSampleCount / 2 - 1].elapsed_ns + samples[kSampleCount / 2].elapsed_ns) / 2;
        constexpr std::size_t p95_index = (kSampleCount * 95 + 99) / 100 - 1;
        const auto allocation_total = std::accumulate(
            samples.begin(), samples.end(), std::size_t{0},
            [](std::size_t total, const Sample &value) { return total + value.allocations; });
        std::cout << "summary," << name << ',' << count << ',' << contexts << ',' << median << ','
                  << samples[p95_index].elapsed_ns << ',' << allocation_total << '\n';
    }

    class BenchmarkPane final : public lux::object::Object<BenchmarkPane, lux::ui::Pane>
    {
      public:
        BenchmarkPane(lux::object::ObjectDispatcherRef dispatcher, std::size_t index)
            : Object(std::move(dispatcher), lux::ui::PaneId{"pane." + std::to_string(index)},
                     lux::ui::PaneTypeId{"benchmark.pane"}, "Pane " + std::to_string(index))
        {
        }

        [[nodiscard]] std::span<const lux::ui::UiContextIdView> contexts() const noexcept override
        {
            return contexts_;
        }

      protected:
        void draw(lux::ui::PaneDrawContext &context) override
        {
            context.activateContext(contexts_.front());
            ImGui::TextUnformatted("benchmark");
        }

      private:
        std::array<lux::ui::UiContextIdView, 1> contexts_{
            lux::ui::UiContextIdView{"benchmark.context"}};
    };

    class CommandReceiver final : public lux::object::Object<CommandReceiver>
    {
      public:
        void invoke() noexcept
        {
            ++invocations;
        }
        [[nodiscard]] bool enabled() const noexcept
        {
            return true;
        }
        [[nodiscard]] bool checked() const noexcept
        {
            return false;
        }
        std::size_t invocations{0};
    };

    void drawFrame(lux::ui::UISession &session)
    {
        session.beginFrame({1280.0F, 720.0F}, 1.0F / 60.0F);
        session.drawPanes();
        static_cast<void>(session.endFrame());
    }

    void benchmarkPanes(std::size_t pane_count)
    {
        lux::ui::UISession session;
        std::vector<std::unique_ptr<BenchmarkPane>> panes;
        std::vector<lux::ui::PaneRegistration> registrations;
        panes.reserve(pane_count);
        registrations.reserve(pane_count);
        for (std::size_t index = 0; index < pane_count; ++index)
        {
            auto pane = std::make_unique<BenchmarkPane>(session.dispatcherRef(), index);
            auto registration = session.registerPane(*pane);
            LUX_CHECK(registration);
            registrations.push_back(std::move(*registration));
            panes.push_back(std::move(pane));
        }

        for (int frame = 0; frame < 3; ++frame)
            drawFrame(session);
        const auto steady_rebuilds =
            lux::ui::detail::CommandRouterDiagnosticsAccess::rebuildCount(session.commandRouter());
        const auto steady_growth =
            lux::ui::detail::UISessionDiagnosticsAccess::wrapperGrowthCount(session);
        benchmark("ui_steady_frame", pane_count, 0, [&] { drawFrame(session); });
        LUX_CHECK(lux::ui::detail::CommandRouterDiagnosticsAccess::rebuildCount(
                      session.commandRouter()) == steady_rebuilds);
        LUX_CHECK(lux::ui::detail::UISessionDiagnosticsAccess::wrapperGrowthCount(session) ==
                  steady_growth);

        if (pane_count >= 2)
        {
            std::size_t focused = 0;
            benchmark("ui_focus_transition", pane_count, 1, [&] {
                focused = 1 - focused;
                LUX_CHECK(session.requestFocus(panes[focused]->id().view()));
                drawFrame(session);
            });
        }
    }

    struct CommandFixture final
    {
        lux::ui::CommandRouter router;
        CommandReceiver scope;
        CommandReceiver alternate_scope;
        CommandReceiver receiver;
        std::vector<lux::ui::CommandHandle> commands;
        std::vector<lux::ui::CommandRegistration> registrations;
        std::vector<lux::ui::UiContextId> context_ids;
        std::vector<lux::ui::UiContextIdView> active_contexts;
        std::vector<lux::ui::UiContextIdView> alternate_contexts;

        CommandFixture(std::size_t binding_count, std::size_t context_count)
        {
            context_ids.reserve(context_count > 0 ? context_count - 1 : 0);
            for (std::size_t index = 0; index + 1 < context_count; ++index)
                context_ids.emplace_back("context." + std::to_string(index));
            for (const auto &context : context_ids)
                active_contexts.push_back(context.view());
            active_contexts.push_back(lux::ui::kGlobalContext);
            alternate_contexts = active_contexts;
            if (alternate_contexts.size() > 2)
                std::swap(alternate_contexts[0], alternate_contexts[1]);

            const auto command_count = context_count == 1 ? binding_count : binding_count / 2;
            commands.reserve(command_count);
            registrations.reserve(binding_count);
            for (std::size_t index = 0; index < command_count; ++index)
            {
                auto command = router.defineCommand(
                    {lux::ui::UiCommandId{"command." + std::to_string(index)}, "Command"});
                LUX_CHECK(command);
                commands.push_back(*command);
                auto global = router.bindGlobal<&CommandReceiver::invoke, &CommandReceiver::enabled,
                                                &CommandReceiver::checked>(*command, receiver);
                LUX_CHECK(global);
                registrations.push_back(std::move(*global));
                if (context_count > 1)
                {
                    const auto &context = context_ids[index % context_ids.size()];
                    auto contextual =
                        router.bind<&CommandReceiver::invoke, &CommandReceiver::enabled,
                                    &CommandReceiver::checked>(
                            *command, lux::ui::UiContextId{context.name()}, scope, receiver);
                    LUX_CHECK(contextual);
                    registrations.push_back(std::move(*contextual));
                }
            }
            lux::ui::detail::CommandRouterDiagnosticsAccess::updateRoute(router, &scope,
                                                                         active_contexts);
        }
    };

    void benchmarkCommands(std::size_t binding_count, std::size_t context_count,
                           std::size_t query_count)
    {
        CommandFixture fixture{binding_count, context_count};
        const auto rebuilds_before =
            lux::ui::detail::CommandRouterDiagnosticsAccess::rebuildCount(fixture.router);
        lux::ui::detail::CommandRouterDiagnosticsAccess::updateRoute(fixture.router, &fixture.scope,
                                                                     fixture.active_contexts);
        LUX_CHECK(lux::ui::detail::CommandRouterDiagnosticsAccess::rebuildCount(fixture.router) ==
                  rebuilds_before);

        const auto query_growth =
            lux::ui::detail::CommandRouterDiagnosticsAccess::storageGrowthCount(fixture.router);
        benchmark("ui_command_state", binding_count, context_count, [&] {
            for (std::size_t index = 0; index < query_count; ++index)
            {
                const auto command = fixture.commands[index % fixture.commands.size()];
                LUX_CHECK(fixture.router.state(command).enabled);
            }
        });
        LUX_CHECK(lux::ui::detail::CommandRouterDiagnosticsAccess::storageGrowthCount(
                      fixture.router) == query_growth);
        benchmark("ui_command_invoke", binding_count, context_count, [&] {
            for (std::size_t index = 0; index < query_count; ++index)
            {
                const auto command = fixture.commands[index % fixture.commands.size()];
                LUX_CHECK(fixture.router.invoke(command) ==
                          lux::ui::ECommandDispatchResult::EXECUTED);
            }
        });
        LUX_CHECK(lux::ui::detail::CommandRouterDiagnosticsAccess::storageGrowthCount(
                      fixture.router) == query_growth);

        bool alternate = false;
        benchmark("ui_route_rebuild", binding_count, context_count, [&] {
            alternate = !alternate;
            lux::ui::detail::CommandRouterDiagnosticsAccess::updateRoute(
                fixture.router,
                alternate ? static_cast<lux::object::LuxObject *>(&fixture.alternate_scope)
                          : static_cast<lux::object::LuxObject *>(&fixture.scope),
                alternate ? std::span<const lux::ui::UiContextIdView>{fixture.alternate_contexts}
                          : std::span<const lux::ui::UiContextIdView>{fixture.active_contexts});
        });
    }

    void benchmarkBindingChurn(std::size_t command_count)
    {
        lux::ui::CommandRouter router;
        CommandReceiver receiver;
        std::vector<lux::ui::CommandHandle> commands;
        commands.reserve(command_count);
        for (std::size_t index = 0; index < command_count; ++index)
        {
            auto command = router.defineCommand(
                {lux::ui::UiCommandId{"churn." + std::to_string(index)}, "Churn"});
            LUX_CHECK(command);
            commands.push_back(*command);
        }
        benchmark("ui_binding_churn", command_count, 1, [&] {
            for (const auto command : commands)
            {
                auto registration = router.bindGlobal<&CommandReceiver::invoke>(command, receiver);
                LUX_CHECK(registration);
            }
        });
    }

    void benchmarkDragPayload(std::size_t payload_size)
    {
        std::array<std::byte, lux::ui::detail::kInlineDragDropBytes> inline_storage{};
        std::vector<std::byte> heap_storage;
        std::vector<std::byte> payload(payload_size, std::byte{0x5});
        constexpr std::size_t iterations = 1'000;
        benchmark("ui_drag_payload", payload_size, 0, [&] {
            for (std::size_t index = 0; index < iterations; ++index)
            {
                const auto encoded = lux::ui::detail::encodeDragDropPayload(
                    lux::ui::PayloadTypeIdView{"benchmark.payload"}, payload, inline_storage,
                    heap_storage);
                LUX_CHECK(lux::ui::detail::decodeDragDropPayload(encoded));
            }
        });
        if (payload_size + 64 < inline_storage.size())
            LUX_CHECK(heap_storage.empty());
    }
} // namespace

void *operator new(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void *memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void *operator new[](std::size_t size)
{
    return ::operator new(size);
}
void *operator new(std::size_t size, std::align_val_t alignment)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
#if defined(_MSC_VER)
    if (void *memory = _aligned_malloc(size, static_cast<std::size_t>(alignment)))
        return memory;
#else
    const auto value = static_cast<std::size_t>(alignment);
    const auto rounded = (size + value - 1) / value * value;
    if (void *memory = std::aligned_alloc(value, rounded))
        return memory;
#endif
    throw std::bad_alloc{};
}
void *operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}
void operator delete(void *memory) noexcept
{
    std::free(memory);
}
void operator delete(void *memory, std::size_t) noexcept
{
    std::free(memory);
}
void operator delete[](void *memory) noexcept
{
    std::free(memory);
}
void operator delete[](void *memory, std::size_t) noexcept
{
    std::free(memory);
}
void operator delete(void *memory, std::align_val_t) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}
void operator delete(void *memory, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}
void operator delete[](void *memory, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}
void operator delete[](void *memory, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}

int main()
{
    std::cout << "meta,warmups," << kWarmupCount << '\n';
    std::cout << "meta,samples," << kSampleCount << '\n';
    std::cout << "record,case,count,contexts,sample_or_median,elapsed_or_p95,"
                 "process_allocations\n";
    for (const auto pane_count : {10U, 50U, 200U})
        benchmarkPanes(pane_count);
    for (const auto binding_count : {100U, 500U, 2000U})
    {
        for (const auto context_count : {1U, 3U, 8U})
            benchmarkCommands(binding_count, context_count, 200);
    }
    benchmarkBindingChurn(100);
    benchmarkBindingChurn(500);
    benchmarkDragPayload(32);
    benchmarkDragPayload(1024);
}
