#include "Discovery.hpp"
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include "../pinclude/TypedProtocol.hpp"
#include "../../resource/test/CpuFixture.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>

using namespace lux::render;
using namespace std::chrono_literals;
namespace ipc = lux::platform::ipc;
namespace detail = lux::render::runtime_detail;

namespace lux::render { class RenderServer; }

static void check(bool value, const char* reason)
{
    if (value) return;
    std::fprintf(stderr, "Server DLL: %s\n", reason);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

template <class T> static T take(RenderResult<T> result)
{
    check(bool(result), "unexpected Render error");
    return std::move(*result);
}

static RenderCompletion wait(RenderClient& client, RequestId id)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        RenderCompletion completion;
        const auto state = take(client.tryReceive(completion));
        if (state == ERenderReceive::RECEIVED)
        {
            check(completion.header.request == id, "wrong source request");
            check(completion.header.outcome == ECommandOutcome::SUCCESS, "request did not succeed");
            return completion;
        }
        check(state != ERenderReceive::CLOSED, "server closed before publishing its result");
        std::this_thread::sleep_for(1ms);
    }
    check(false, "request timed out");
    return {};
}

enum class EReplyCase { SUCCESS, HELLO, FAILURE_REPLY, EXTRACTED_READBACK };

static void lateReply(EReplyCase mode)
{
    const auto module = LoadLibraryW(L"render_server_dll_plugin.dll");
    check(module != nullptr, "plugin load failed");
    unsigned unloaded = 0;
    auto pin = std::shared_ptr<const void>(module, [&](const void* value) noexcept {
        check(FreeLibrary(static_cast<HMODULE>(const_cast<void*>(value))) != 0, "plugin unload failed");
        ++unloaded;
    });
    using Create = RenderServer* (*)(const char*, const std::shared_ptr<const void>*, const CpuSchedule*) noexcept;
    using Readback = bool (*)(const char*, RenderPayload*) noexcept;
    using Run = bool (*)(RenderServer*) noexcept;
    using Stop = void (*)(RenderServer*) noexcept;
    using Destroy = void (*)(RenderServer*) noexcept;
    const auto create = reinterpret_cast<Create>(GetProcAddress(module, "serverDllCreate"));
    const auto run = reinterpret_cast<Run>(GetProcAddress(module, "serverDllRun"));
    const auto stop = reinterpret_cast<Stop>(GetProcAddress(module, "serverDllStop"));
    const auto destroy = reinterpret_cast<Destroy>(GetProcAddress(module, "serverDllDestroy"));
    const auto readback = reinterpret_cast<Readback>(GetProcAddress(module, "serverDllReadback"));
    check(create && run && stop && destroy && readback, "plugin exports missing");

    std::optional<test::CpuFixture> cpu;
    if (mode == EReplyCase::EXTRACTED_READBACK) cpu.emplace();
    const auto scheduler = cpu ? cpu->cpu() : CpuSchedule{};
    auto* server = create("runtime.server.dll", &pin, &scheduler);
    check(server != nullptr, "plugin server create failed");
    bool run_ok = false;
    std::thread worker([&] { run_ok = run(server); });
    std::optional<TypedRenderCompletion<CreateScene>> success;
    RenderCompletion hello;
    ipc::IpcReply error;
    RenderPayload extracted;
    std::vector<std::byte> extracted_snapshot;
    if (mode == EReplyCase::EXTRACTED_READBACK)
    {
        check(readback("runtime.server.dll", &extracted), "real GPU Readback helper failed");
        const auto bytes = extracted.bytes().view();
        check(bytes.size() == 32 * 32 * 4, "initial Readback has incorrect dimensions");
        extracted_snapshot.assign(bytes.begin(), bytes.end());
        std::printf("Server DLL: Readback before destruction first pixel = %u,%u,%u,%u\n",
            std::to_integer<unsigned>(bytes[0]), std::to_integer<unsigned>(bytes[1]),
            std::to_integer<unsigned>(bytes[2]), std::to_integer<unsigned>(bytes[3]));
        std::fflush(stdout);
        constexpr std::array expected{32, 64, 128, 255};
        for (std::size_t i = 0; i < bytes.size(); ++i)
            check(std::abs(std::to_integer<int>(bytes[i]) - expected[i % 4]) <= 1,
                "initial real GPU Readback failed the existing UNORM pixel oracle");
    }
    else if (mode == EReplyCase::FAILURE_REPLY)
    {
        // The original request is EXE-owned and carries no Server pin. This isolates the error
        // message's own implementation pin, without a builtin request masking a missing one.
        auto peer_result = ipc::IpcCore::instance().createRequester("runtime.server.dll");
        check(bool(peer_result), "raw requester create failed");
        auto peer = std::move(*peer_result);
        ipc::IpcMessage input;
        input.emplace<std::uint64_t>(0);
        const RequestId request{3, 1};
        input.setMetadata(detail::RequestMetadata{{}, request, CommandId{UINT32_MAX - 1, 1}, 128});
        check(bool(peer.trySend(input)), "raw request publication failed");
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (peer.tryReceive(error) == ipc::EIpcReceive::RECEIVED) break;
            std::this_thread::sleep_for(1ms);
        }
        const auto* operations = error.message.operations();
        check(operations && bool(ipc::IpcMessage::bindType<detail::ValueEnvelope<RenderError>>(*operations)),
            "raw error did not satisfy its complete cold type contract");
        const auto metadata = error.message.metadata<detail::ReplyMetadata>();
        check(metadata.request == request && metadata.outcome == ECommandOutcome::FAILED &&
            error.message.get<detail::ValueEnvelope<RenderError>>().value.type == kRuntimeIdentity,
            "invalid raw identity did not produce its real error terminal");
        peer.reset();
    }
    else
    {
        auto client = take(RenderClient::create("runtime.server.dll"));
        auto greeting = wait(*client, take(client->hello()));
        if (mode == EReplyCase::HELLO) hello = std::move(greeting);
        else
        {
            greeting = {};
            check(bool(runtime_test::refresh(*client)), "explicit business discovery failed");
            auto binding = take(client->bind<CreateScene>());
            CreateScene::Request request{};
            auto completion = wait(*client, take(client->submit(binding, request)));
            success.emplace(take(std::move(completion).take(binding)));
            check(!success->response().scene.isNull(), "real builtin CreateScene produced no identity");
        }
        client.reset();
    }

    stop(server);
    worker.join();
    check(run_ok, "plugin server failed while running");
    destroy(server);
    cpu.reset(); // The real scheduler's accepted callback tails finish while the host still pins the module.
    pin.reset();
    check(unloaded == 0 && GetModuleHandleW(L"render_server_dll_plugin.dll") != nullptr,
        "the last Reply did not retain Server implementation code");
    if (success) check(!success->response().scene.isNull(), "late success became inaccessible");
    if (mode == EReplyCase::HELLO) check(hello.header.outcome == ECommandOutcome::SUCCESS, "late Hello changed");
    if (mode == EReplyCase::FAILURE_REPLY)
        check(error.message.get<detail::ValueEnvelope<RenderError>>().value.type == kRuntimeIdentity,
            "late error became inaccessible");
    if (mode == EReplyCase::EXTRACTED_READBACK)
    {
        const auto bytes = extracted.bytes().view();
        check(bytes.size() == 32 * 32 * 4, "extracted Readback lost its owned bytes");
        std::printf("Server DLL: Readback after destruction first pixel = %u,%u,%u,%u\n",
            std::to_integer<unsigned>(bytes[0]), std::to_integer<unsigned>(bytes[1]),
            std::to_integer<unsigned>(bytes[2]), std::to_integer<unsigned>(bytes[3]));
        std::fflush(stdout);
        check(std::equal(bytes.begin(), bytes.end(), extracted_snapshot.begin(), extracted_snapshot.end()),
            "extracted Readback changed bytes after Server/Reply destruction");
    }
    success.reset();
    hello = {};
    error = {};
    extracted = {};
    check(unloaded == 1 && GetModuleHandleW(L"render_server_dll_plugin.dll") == nullptr,
        "final Reply destruction did not release the module exactly once after its thunk returned");
    std::printf("Server DLL: real late Reply case %u retained code and unloaded exactly once PASS\n", unsigned(mode));
}

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    lateReply(EReplyCase::SUCCESS);
    lateReply(EReplyCase::HELLO);
    lateReply(EReplyCase::FAILURE_REPLY);
    lateReply(EReplyCase::EXTRACTED_READBACK);
}
