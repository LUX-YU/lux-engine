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
        // Let initial output construction settle before observing quiet idle.
        const auto warm_until = std::chrono::steady_clock::now() + 250ms;
        do
        {
            assert(host->frame());
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < warm_until);
        assert(probe->changed);
        const auto before = host->statistics();
        const auto started = std::chrono::steady_clock::now();
        process::Task task;
        std::jthread close;
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
        assert(host->exec());
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto after = host->statistics();
        const auto iterations = after.iterations - before.iterations;
        const auto frames = after.captured_frames - before.captured_frames;
        const auto waits = after.waits - before.waits;
        const auto wait_ms = std::chrono::duration<double, std::milli>(after.wait - before.wait).count();
        std::printf(
            "PACING vsync=%d minimized=%d native=%d iterations=%llu frames=%llu waits=%llu wait_ms=%.3f "
            "elapsed_ms=%.3f active_ms=%.3f backpressure_waits=%llu\n",
            vsync,
            minimized,
            native_close,
            iterations,
            frames,
            waits,
            wait_ms,
            std::chrono::duration<double, std::milli>(elapsed).count(),
            std::chrono::duration<double, std::milli>(after.total - before.total).count(),
            after.backpressure_waits - before.backpressure_waits
        );
        assert(elapsed >= 650ms && elapsed < 3s);
        assert(native_close || receiver.called);
        assert(native_close || after.object_messages > before.object_messages);
        assert(waits > 0); // Real blocking path, not just a predicate unit test.
        if (minimized)
        {
            assert(frames == 0 && iterations < 30 && wait_ms > 400);
        }
        else
        {
            assert(after.backpressure_waits > before.backpressure_waits);
            assert(frames > 0 && after.ui_draw > before.ui_draw && after.ui_capture > before.ui_capture);
        }
        assert(after.scenes == 1 && after.ui.panes == 1 && after.ui.elements == 1);
        assert(host->engine().renderContext()->runtime().statistics().frames > 0 || minimized);
    }
    void closeDuringMaintenance()
    {
        auto made = editor::LuxEngine::create({"Close during maintenance", 640, 480});
        assert(made);
        auto host = std::move(*made);
        auto pane = std::make_unique<Probe>();
        pane->close_window = &host->window();
        std::unique_ptr<ui::Pane> owner = std::move(pane);
        assert(host->window().uiRoot().addPane(std::move(owner)));
#if defined(_WIN32)
        ShowWindow(static_cast<HWND>(host->window().nativeHandle()), SW_MINIMIZE);
#endif
        assert(host->exec());
        assert(host->statistics().iterations <= 2);
        assert(host->statistics().waits == 0);
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
