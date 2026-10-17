#include "../Discovery.hpp"
#include <lux/engine/render/runtime/Commands.hpp>
#include "Fixture.hpp"
#include "../../../resource/test/CpuFixture.hpp"
#include <lux/engine/render/runtime/VulkanServer.hpp>

#define NOMINMAX
#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace lux::render;
using namespace async_tail_test;
using namespace std::chrono_literals;

static void check(bool value, const char* reason) noexcept
{
    if (value) return;
    std::fprintf(stderr, "async DLL tail: %s\n", reason);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

template <class T> static T take(RenderResult<T> value)
{
    check(bool(value), "unexpected Render error");
    return std::move(*value);
}

template <class Predicate> static void until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!predicate())
    {
        check(std::chrono::steady_clock::now() < deadline, "timed out");
        std::this_thread::yield();
    }
}

static void exercise(const char* library, ETerminal terminal)
{
    const auto module = LoadLibraryA(library);
    check(module != nullptr, "LoadLibrary failed");
    std::atomic<unsigned> unloaded{0};
    auto pin = std::shared_ptr<const void>(module, [&](const void* pointer) noexcept {
        check(FreeLibrary(static_cast<HMODULE>(const_cast<void*>(pointer))) != 0, "FreeLibrary failed");
        unloaded.fetch_add(1, std::memory_order_release);
    });
    const auto make = reinterpret_cast<MakeFeature>(GetProcAddress(module, "makeAsyncTailFeature"));
    check(make != nullptr, "provider export missing");

    auto cpu = std::make_unique<lux::render::test::CpuFixture>(1, 16);
    Gate gate{terminal};
    Host host{cpu->cpu(), &gate};
    RenderServerConfig config{"runtime.async.dll_callback_tail", true};
    auto server = take(vulkan::createRenderServer(std::move(config), cpu->runtime()));
    RenderResult<FeatureId> installed = lux::cxx::unexpected(RenderError{kRuntimeState});
    make(*server, host, installed);
    check(bool(installed), "Feature registration failed");
    const auto catalog = take(server->commandId<ListCommands>());
    std::atomic<bool> finished{false};
    bool run_ok = false;
    std::thread render([&] {
        run_ok = bool(server->run());
        finished.store(true, std::memory_order_release);
    });
    {
        auto client = take(runtime_test::connect("runtime.async.dll_callback_tail", catalog));
        check(bool(runtime_test::refresh(*client)), "explicit discovery failed");
        auto binding = take(runtime_test::find<Command>(*client));
        std::uint32_t request = 4097;
        auto reply = runtime_test::wait(*client, take(runtime_test::send<Command>(*client, binding, request)));
        if (terminal == ETerminal::VALUE)
        {
            check(reply.get<Command::Response>() == 4097, "typed response changed");
        }
        else if (terminal == ETerminal::FAILURE)
        {
            check(reply.metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED &&
                reply.get<RenderError>() == RenderError{kProtocolContract, {73}}, "error terminal changed");
        }
        else check(reply.metadata<ReplyHeader>().outcome == ECommandOutcome::CANCELLED, "stopped terminal changed");
        check(gate.published.try_acquire_for(15s), "provider did not enter its callback tail");
        // Reply storage ends before Server shutdown; the Host still owns the loaded module.
    }

    server->stop();
    until([&] { return finished.load(std::memory_order_acquire); });
    render.join();
    check(run_ok, "Server failed while stopping");
    check(server->stats().validation_errors == 0, "GPU validation error");
    server.reset();
    check(gate.states_destroyed.load(std::memory_order_acquire) == 1,
        "operation state was not destroyed before the callback returned");
    check(unloaded.load(std::memory_order_acquire) == 0 &&
        GetModuleHandleA("render_async_tail_plugin.dll") != nullptr,
        "provider unloaded while its terminal callback was still executing");
    check(gate.tails_returned.load(std::memory_order_acquire) == 0, "callback tail was not held");

    gate.release.release();
    // This is the actual scheduler owner's stop/join, not another test pool or
    // an operation-state polling loop. It waits for the worker callback to return.
    cpu.reset();
    check(gate.tails_returned.load(std::memory_order_acquire) == 1, "callback did not return exactly once");
    pin.reset(); // Host unload occurs only after the actual worker callback has returned.
    check(unloaded.load(std::memory_order_acquire) == 1 &&
        GetModuleHandleA("render_async_tail_plugin.dll") == nullptr,
        "final host callback return did not release its module exactly once");
    std::printf("async DLL tail: terminal=%u state_destroyed=1 tail_returned=1 module_unloaded=1\n",
        static_cast<unsigned>(terminal));
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    check(argc == 2, "expected provider DLL path");
    exercise(argv[1], ETerminal::VALUE);
    exercise(argv[1], ETerminal::FAILURE);
    exercise(argv[1], ETerminal::STOPPED);
}
