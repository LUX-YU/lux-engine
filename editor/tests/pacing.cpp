#include <algorithm>
#include <cassert>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/RenderContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
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
    struct Probe final : ui::Pane
    {
        Probe() : Pane("Pacing probe"), label("Actual rendered content")
        {
            assert(addElement(label));
        }
        void update() noexcept override
        {
            ++updates;
            if (close_window)
            {
                close_window->exit();
            }
            if (!scheduled)
            {
                scheduled = true;
                root().deferChange(
                    *this,
                    [](object::LuxObject& target) noexcept { static_cast<Probe&>(target).changed = true; }
                );
            }
        }
        ui::Label label;
        window::LuxWindow* close_window{};
        std::size_t updates{};
        bool scheduled{}, changed{};
    };
    struct Receiver final : object::LuxObject
    {
        explicit Receiver(editor::LuxEngine& value) : host(value) {}
        object::TSignal<> finished{*this};
        editor::LuxEngine& host;
        bool called{};
        void complete() noexcept
        {
            called = true;
            host.window().exit();
        }
        void queue() noexcept
        {
            const auto sent = emit(finished);
            assert(sent.queued == 1);
        }
    };
    auto timer(editor::LuxEngine& host, Receiver& receiver)
    {
        auto& runtime = host.engine().execution();
        using Result = cxx::expected<void, process::ETimerError>;
        return runtime.submit(
            {"Pacing completion", "qualification"},
            [&](process::TaskReporter) noexcept
            {
                return stdexec::upon_error(
                    stdexec::then(runtime.timer().after(700ms), []() noexcept { return Result{}; }),
                    [](process::ETimerError error) noexcept -> Result { return cxx::unexpected(error); }
                );
            },
            [&](process::TTaskResult<void, process::ETimerError>&& result) noexcept
            {
                assert(result);
                receiver.queue(); // Work from dispatch reaches the existing Object batch.
            }
        );
    }
    void run(bool vsync, bool minimized, bool native_close)
    {
        auto made = editor::LuxEngine::create({"Framework pacing", 640, 480, vsync});
        assert(made);
        auto host = std::move(*made);
        auto pane = std::make_unique<Probe>();
        auto* probe = pane.get();
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
        std::chrono::steady_clock::time_point started;
        process::Task task;
        std::jthread close;
        bool warmed{};
        auto& execution = host->engine().execution();
        auto warm = execution.submit(
            {"Pacing warmup", "qualification"},
            [&](process::TaskReporter) noexcept
            {
                return stdexec::upon_error(
                    stdexec::then(
                        execution.timer().after(250ms),
                        []() noexcept { return cxx::expected<void, process::ETimerError>{}; }
                    ),
                    [](process::ETimerError error) noexcept -> cxx::expected<void, process::ETimerError>
                    { return cxx::unexpected(error); }
                );
            },
            [&](process::TTaskResult<void, process::ETimerError>&& result) noexcept
            {
                assert(result && probe->changed);
                warmed = true;
                before_updates = probe->updates;
                before_frames = host->engine().renderContext()->runtime().statistics().frames;
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
            }
        );
        assert(warm);
        assert(host->run());
        assert(warmed);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto iterations = probe->updates - before_updates;
        const auto frames = host->engine().renderContext()->runtime().statistics().frames - before_frames;
        std::printf(
            "PACING vsync=%d minimized=%d native=%d updates=%llu rendered=%llu elapsed_ms=%.3f\n",
            vsync,
            minimized,
            native_close,
            static_cast<unsigned long long>(iterations),
            static_cast<unsigned long long>(frames),
            std::chrono::duration<double, std::milli>(elapsed).count()
        );
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
        assert(host->engine().sceneRuntime().instanceCount() == 1);
        assert(host->window().uiRoot().statistics().panes == 1);
        assert(host->window().uiRoot().statistics().elements == 1);
    }
    void closeDuringMaintenance()
    {
        auto made = editor::LuxEngine::create({"Close during maintenance", 640, 480});
        assert(made);
        auto host = std::move(*made);
        auto pane = std::make_unique<Probe>();
        auto* probe = pane.get();
        pane->close_window = &host->window();
        std::unique_ptr<ui::Pane> owner = std::move(pane);
        assert(host->window().uiRoot().addPane(std::move(owner)));
#if defined(_WIN32)
        ShowWindow(static_cast<HWND>(host->window().nativeHandle()), SW_MINIMIZE);
#endif
        assert(host->run());
        assert(probe->updates <= 2);
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
