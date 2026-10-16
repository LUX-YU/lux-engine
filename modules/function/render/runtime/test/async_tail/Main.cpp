#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include "Fixture.hpp"
#include "../../../resource/test/CpuFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>

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

static RenderCompletion wait(RenderClient& client, RequestId id)
{
    RenderCompletion result;
    until([&] {
        const auto state = take(client.tryReceive(result));
        check(state != ERenderReceive::CLOSED, "endpoint closed before its terminal reply");
        return state == ERenderReceive::RECEIVED;
    });
    check(result.header.request == id, "reply crossed request identity");
    return result;
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
    Host host{cpu->cpu(), pin, &gate, {}};
    RenderServerConfig config{"runtime.async.dll_callback_tail", true};
    config.cpu = cpu->cpu();
    auto server = take(RenderServer::create(std::move(config)));
    FeatureRegistration registration{};
    make(host, pin, registration);
    check(bool(server->registerFeature<Command>(std::move(registration))), "Feature registration failed");
    std::atomic<bool> finished{false};
    bool run_ok = false;
    std::thread render([&] {
        run_ok = bool(server->run());
        finished.store(true, std::memory_order_release);
    });
    {
        auto client = take(RenderClient::create("runtime.async.dll_callback_tail"));
        {
            auto hello = wait(*client, take(client->hello()));
            check(hello.header.outcome == ECommandOutcome::SUCCESS, "Hello failed");
        }
        auto binding = take(client->bind<Command>());
        std::uint32_t request = 4097;
        auto reply = wait(*client, take(client->submit(binding, request)));
        if (terminal == ETerminal::VALUE)
        {
            auto typed = take(std::move(reply).take(binding));
            check(typed.response() == 4097, "typed response changed");
        }
        else if (terminal == ETerminal::FAILURE)
        {
            check(reply.header.outcome == ECommandOutcome::FAILED &&
                reply.header.error == RenderError{kProtocolContract, {73}}, "error terminal changed");
        }
        else check(reply.header.outcome == ECommandOutcome::CANCELLED, "stopped terminal changed");
        check(gate.published.try_acquire_for(15s), "provider did not enter its callback tail");
        // All Reply, original Request, bound type and manifest code pins are
        // destroyed with this scope, before checking the callback's own pin.
    }

    server->stop();
    until([&] { return finished.load(std::memory_order_acquire); });
    render.join();
    check(run_ok, "Server failed while stopping");
    check(server->stats().validation_errors == 0, "GPU validation error");
    server.reset();
    check(gate.states_destroyed.load(std::memory_order_acquire) == 1,
        "operation state was not destroyed before the callback returned");
    pin.reset();
    check(unloaded.load(std::memory_order_acquire) == 0 && !host.module.expired() &&
        GetModuleHandleA("render_async_tail_plugin.dll") != nullptr,
        "provider unloaded while its terminal callback was still executing");
    check(gate.tails_returned.load(std::memory_order_acquire) == 0, "callback tail was not held");

    gate.release.release();
    // This is the actual scheduler owner's stop/join, not another test pool or
    // an operation-state polling loop. It waits for the worker callback to return.
    cpu.reset();
    check(gate.tails_returned.load(std::memory_order_acquire) == 1, "callback did not return exactly once");
    check(unloaded.load(std::memory_order_acquire) == 1 && host.module.expired() &&
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
