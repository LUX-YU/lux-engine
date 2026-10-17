#include "Discovery.hpp"
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
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

enum class EReplyCase { SUCCESS, DIRECTORY, FAILURE_REPLY, EXTRACTED_READBACK };

static void lateReply(EReplyCase mode)
{
    const auto module = LoadLibraryW(L"render_server_dll_plugin.dll");
    check(module != nullptr, "plugin load failed");
    using Create = RenderServer* (*)(const char*, const CpuSchedule*) noexcept;
    using Readback = bool (*)(const char*, CommandId, RenderPayload*) noexcept;
    using Directory = CommandId (*)(RenderServer*) noexcept;
    using Run = bool (*)(RenderServer*) noexcept;
    using Stop = void (*)(RenderServer*) noexcept;
    using Destroy = void (*)(RenderServer*) noexcept;
    const auto create = reinterpret_cast<Create>(GetProcAddress(module, "serverDllCreate"));
    const auto run = reinterpret_cast<Run>(GetProcAddress(module, "serverDllRun"));
    const auto stop = reinterpret_cast<Stop>(GetProcAddress(module, "serverDllStop"));
    const auto destroy = reinterpret_cast<Destroy>(GetProcAddress(module, "serverDllDestroy"));
    const auto readback = reinterpret_cast<Readback>(GetProcAddress(module, "serverDllReadback"));
    const auto directory = reinterpret_cast<Directory>(GetProcAddress(module, "serverDllDirectory"));
    check(create && run && stop && destroy && readback && directory, "plugin exports missing");

    std::optional<test::CpuFixture> cpu;
    if (mode == EReplyCase::EXTRACTED_READBACK) cpu.emplace();
    const auto scheduler = cpu ? cpu->cpu() : CpuSchedule{};
    auto* server = create("runtime.server.dll", &scheduler);
    check(server != nullptr, "plugin server create failed");
    const auto catalog = directory(server);
    check(!catalog.isNull(), "cold directory ID missing");
    bool run_ok = false;
    std::thread worker([&] { run_ok = run(server); });
    ipc::IpcMessage success, listing, error;
    RenderPayload extracted;
    std::vector<std::byte> extracted_snapshot;
    if (mode == EReplyCase::EXTRACTED_READBACK)
    {
        check(readback("runtime.server.dll", catalog, &extracted), "real GPU Readback helper failed");
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
        auto peer_result = ipc::IpcCore::instance().createRequester("runtime.server.dll");
        check(bool(peer_result), "raw requester create failed");
        auto peer = std::move(*peer_result);
        ipc::IpcMessage input;
        input.emplace<std::uint64_t>(0);
        const RequestId request{3};
        input.setMetadata(RequestHeader{request, CommandId{UINT32_MAX - 1, 1}});
        check(bool(peer.trySend(input)), "raw request publication failed");
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (peer.tryReceive(error) == ipc::EIpcReceive::RECEIVED) break;
            std::this_thread::sleep_for(1ms);
        }
        const auto metadata = error.metadata<ReplyHeader>();
        check(metadata.request == request && metadata.outcome == ECommandOutcome::FAILED &&
            error.get<RenderError>().type == kRuntimeIdentity, "invalid raw identity did not produce its real error");
        peer.reset();
    }
    else
    {
        auto client = take(runtime_test::connect("runtime.server.dll", catalog));
        if (mode == EReplyCase::DIRECTORY)
        {
            const auto sent = take(runtime_test::send<ListCommands>(*client, catalog, {}));
            listing = runtime_test::wait(*client, sent);
            check(listing.metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS, "directory failed");
        }
        else
        {
            check(bool(runtime_test::refresh(*client)), "explicit business discovery failed");
            const auto binding = take(runtime_test::find<CreateScene>(*client));
            const auto sent = take(runtime_test::send<CreateScene>(*client, binding, {}));
            success = runtime_test::wait(*client, sent);
            check(!success.get<CreateScene::Response>().scene.isNull(), "CreateScene produced no identity");
        }
        client.reset();
    }

    stop(server);
    worker.join();
    check(run_ok, "plugin server failed while running");
    destroy(server);
    cpu.reset(); // The real scheduler's accepted callback tails finish while the host still pins the module.
    check(GetModuleHandleW(L"render_server_dll_plugin.dll") != nullptr, "host unloaded the live result provider");
    if (!success.empty()) check(!success.get<CreateScene::Response>().scene.isNull(), "late success changed");
    if (mode == EReplyCase::DIRECTORY)
        check(!listing.get<CommandList>().commands.empty(), "late directory lost owned names");
    if (mode == EReplyCase::FAILURE_REPLY)
        check(error.get<RenderError>().type == kRuntimeIdentity, "late error changed");
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
    listing.reset();
    error = {};
    extracted = {};
    check(FreeLibrary(module) != 0, "host module unload failed");
    check(GetModuleHandleW(L"render_server_dll_plugin.dll") == nullptr,
        "host unload occurred before all result destructors returned");
    std::printf("Server DLL: real late Reply case %u survived Server destruction; host unloaded after final result\n", unsigned(mode));
}

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    lateReply(EReplyCase::SUCCESS);
    lateReply(EReplyCase::DIRECTORY);
    lateReply(EReplyCase::FAILURE_REPLY);
    lateReply(EReplyCase::EXTRACTED_READBACK);
}
