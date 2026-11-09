#include "support/ProjectRequests.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <lux/engine/Engine.hpp>
#include <lux/engine/editor/EditorApplication.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/systems/render/RenderExecution.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace lux;
using namespace std::chrono_literals;

namespace
{
    struct ProbeState final
    {
        std::size_t updates{};
        bool changed{}, destroyed{};
    };

    struct Probe final : ui::Pane
    {
        explicit Probe(ProbeState& state) : Pane("Pacing probe"), label("Actual rendered content"), state(state)
        {
            setContinuousUpdate(true);
            assert(addElement(label));
        }

        ~Probe() noexcept override
        {
            state.destroyed = true;
        }

        void update() noexcept override
        {
            ++state.updates;
            if (close_engine)
            {
                close_engine->requestClose();
            }
            if (!scheduled)
            {
                scheduled = true;
                root().deferChange(
                    *this,
                    [](object::LuxObject& target) noexcept { static_cast<Probe&>(target).state.changed = true; }
                );
            }
        }

        ui::Label label;
        engine::Engine* close_engine{};
        ProbeState& state;
        bool scheduled{};
    };

    struct Receiver final : object::LuxObject
    {
        explicit Receiver(editor::EditorApplication& value) : host(value) {}

        object::TSignal<> finished{*this};
        editor::EditorApplication& host;
        bool called{};
        std::function<void()> finish_measurement;

        void complete() noexcept
        {
            called = true;
            finish_measurement();
            host.engine().requestClose();
        }

        void queue() noexcept
        {
            const auto sent = emit(finished);
            assert(sent.queued == 1);
        }
    };

    auto timer(editor::EditorApplication& host, Receiver& receiver)
    {
        auto& runtime = host.engine().execution();
        using Result = cxx::expected<void, execution::ETimerError>;
        return runtime.submit(
            {"Pacing completion", "qualification"},
            [&,
             make_work =
                 [&](execution::TaskReporter) noexcept
             {
                 return stdexec::upon_error(
                     stdexec::then(runtime.timer().after(700ms), []() noexcept { return Result{}; }),
                     [](execution::ETimerError error) noexcept -> Result { return cxx::unexpected(error); }
                 );
             },
             consume_result =
                 [&](execution::ExecutionResult<void, execution::ETimerError>&& result) noexcept
             {
                 assert(result);
                 receiver.queue(); // Work from dispatch reaches the existing Object batch.
             }](lux::execution::TaskReporter reporter) mutable noexcept
            {
                auto ready =
                    stdexec::continues_on(lux::execution::asResult(make_work(reporter)), runtime.mainScheduler());
                return stdexec::then(
                    std::move(ready),
                    [consume_result = std::move(consume_result)](auto result) mutable noexcept
                    {
                        const auto outcome = lux::execution::taskState(result);
                        consume_result(std::move(result));
                        return outcome;
                    }
                );
            }
        );
    }

    void run(bool vsync, bool minimized, bool native_close)
    {
        auto made = fixture::createLoadedEditor(fixture::loadedConfig("Framework pacing", 640, 480, vsync));
        assert(made);
        auto host = std::move(*made);
        ProbeState state;
        auto pane = std::make_unique<Probe>(state);
        std::unique_ptr<ui::Pane> owner = std::move(pane);
        assert(host->window().uiRoot().addPane(std::move(owner)));
        Receiver receiver(*host);
        auto connection = object::LuxObject::connect(
            &receiver,
            &Receiver::finished,
            &receiver,
            &Receiver::complete,
            object::EDelivery::QUEUED
        );
        assert(connection);
        // Project readiness does not promise the first UI presentation. Qualify idle pacing only after it.
        const auto presentation_deadline = std::chrono::steady_clock::now() + 20s;
        while (host->rendering().stats().presentations == 0)
        {
            assert(std::chrono::steady_clock::now() < presentation_deadline);
            assert(fixture::stepEditor(*host));
            std::this_thread::sleep_for(1ms);
        }
#if defined(_WIN32)
        const auto hwnd = static_cast<HWND>(host->window().nativeHandle());
        if (minimized)
        {
            ShowWindow(hwnd, SW_MINIMIZE);
        }
#endif
        // Observe only after a real timer wakes the public event loop. No public
        // frame hook or host-owned profiling state is used by this SDK consumer.
        std::size_t before_updates{}, before_frames{}, before_messages{};
        std::chrono::steady_clock::time_point started, finished;
        std::size_t final_updates{}, final_frames{};
        const auto finish_measurement = [&]() noexcept
        {
            finished = std::chrono::steady_clock::now();
            final_updates = state.updates;
            final_frames = host->rendering().stats().presentations;
        };
        receiver.finish_measurement = finish_measurement;
        host->window().on_close = [&](const window::WindowCloseEvent&) noexcept
        {
            finish_measurement();
            host->engine().requestClose(); // The replacement callback owns the actual close request.
        };
        execution::Task task;
        std::jthread close;
        bool warmed{};
        auto& execution = host->engine().execution();
        auto warm = execution.submit(
            {"Pacing warmup", "qualification"},
            [&,
             make_work =
                 [&](execution::TaskReporter) noexcept
             {
                 return stdexec::upon_error(
                     stdexec::then(
                         execution.timer().after(250ms),
                         []() noexcept { return cxx::expected<void, execution::ETimerError>{}; }
                     ),
                     [](execution::ETimerError error) noexcept -> cxx::expected<void, execution::ETimerError>
                     { return cxx::unexpected(error); }
                 );
             },
             consume_result =
                 [&](execution::ExecutionResult<void, execution::ETimerError>&& result) noexcept
             {
                 assert(result && state.changed);
                 warmed = true;
                 before_updates = state.updates;
                 before_frames = host->rendering().stats().presentations;
                 before_messages = object::ObjectRuntime::instance().statistics().posted;
                 started = std::chrono::steady_clock::now();
                 if (native_close)
                 {
#if defined(_WIN32)
                     close = std::jthread(
                         [hwnd]
                         {
                             std::this_thread::sleep_for(700ms);
                             assert(PostMessageW(hwnd, WM_CLOSE, 0, 0));
                         }
                     );
#endif
                 }
                 else
                 {
                     auto accepted = timer(*host, receiver);
                     assert(accepted);
                     task = std::move(*accepted);
                 }
             }](lux::execution::TaskReporter reporter) mutable noexcept
            {
                auto ready =
                    stdexec::continues_on(lux::execution::asResult(make_work(reporter)), execution.mainScheduler());
                return stdexec::then(
                    std::move(ready),
                    [consume_result = std::move(consume_result)](auto result) mutable noexcept
                    {
                        const auto outcome = lux::execution::taskState(result);
                        consume_result(std::move(result));
                        return outcome;
                    }
                );
            }
        );
        assert(warm);
        assert(host->run());
        assert(warmed);
        const auto elapsed = finished - started;
        const auto iterations = final_updates - before_updates;
        const auto frames = final_frames - before_frames;
        const auto cleanup = std::chrono::steady_clock::now() - finished;
        assert(cleanup < 3s); // Shutdown callbacks are measured separately from the stable idle interval.
        std::printf(
            "PACING vsync=%d minimized=%d native=%d updates=%llu rendered=%llu elapsed_ms=%.3f\n",
            vsync,
            minimized,
            native_close,
            static_cast<unsigned long long>(iterations),
            static_cast<unsigned long long>(frames),
            std::chrono::duration<double, std::milli>(elapsed).count()
        );
        std::fflush(stdout);
        assert(elapsed >= 650ms && elapsed < 3s);
        assert(native_close || receiver.called);
        assert(native_close || object::ObjectRuntime::instance().statistics().posted > before_messages);
        if (minimized)
        {
            assert(frames == 0 && iterations < 30); // Rules out busy polling over the 700ms observation.
        }
        else
        {
            assert(frames > 0 && iterations > 0);
        }
        assert(host->engine().sceneRuntime().instanceCount() == 0);
        // run() now finishes the application close protocol. Measurements outlive the retired Pane.
        assert(state.destroyed);
        // Shutdown shares the normal maintenance path: the last Root update has no retired Pane callbacks.
        assert(host->window().uiRoot().statistics().panes == 0);
        assert(host->window().uiRoot().statistics().elements == 0); // Static Label has no repeated maintenance.
    }

    void closeDuringMaintenance()
    {
        auto made = fixture::createLoadedEditor(fixture::loadedConfig("Close during maintenance", 640, 480));
        assert(made);
        auto host = std::move(*made);
        ProbeState state;
        auto pane = std::make_unique<Probe>(state);
        pane->close_engine = &host->engine();
        std::unique_ptr<ui::Pane> owner = std::move(pane);
        assert(host->window().uiRoot().addPane(std::move(owner)));
#if defined(_WIN32)
        ShowWindow(static_cast<HWND>(host->window().nativeHandle()), SW_MINIMIZE);
#endif
        assert(host->run());
        assert(state.destroyed && state.updates != 0);
        const auto completed_updates = state.updates;
        assert(host->window().uiRoot().update());
        assert(state.updates == completed_updates); // Modal shutdown maintenance never calls a released Pane.
    }
} // namespace

int main()
{
    closeDuringMaintenance();
    run(true, true, false);
    run(false, true, true);
    run(true, false, false);
    run(false, false, false);
}
