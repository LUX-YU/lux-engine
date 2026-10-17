#include "ClientDllFixture.hpp"
#include <Windows.h>
#include <cstdio>

using namespace lux::render;
namespace ipc = lux::platform::ipc;
using client_dll_test::Batch;
static void check(bool value) { if (!value) std::abort(); }

int main()
{
    const auto module = LoadLibraryW(L"render_client_dll_plugin.dll");
    check(module != nullptr);
    using Make = void (*)(ipc::IpcMessage*, unsigned, int*, const bool*) noexcept;
    using Descriptor = const CommandDescriptor* (*)() noexcept;
    using Core = ipc::IpcCore* (*)() noexcept;
    const auto make = reinterpret_cast<Make>(GetProcAddress(module, "clientDllMessage"));
    const auto descriptor = reinterpret_cast<Descriptor>(GetProcAddress(module, "clientDllCommand"));
    const auto core = reinterpret_cast<Core>(GetProcAddress(module, "clientDllCore"));
    check(make && descriptor && core && core() == &ipc::IpcCore::instance());
    check(bool(compatibleCommands(*descriptor(), commandDescriptor<client_dll_test::Command>())));
    check(!compatibleCommands(*descriptor(), commandDescriptor<client_dll_test::WrongCommand>()));
    bool host_loaded = true;
    int destroyed = 0;
    auto requester_result = core()->createRequester("test.direct.dll");
    check(bool(requester_result));
    auto requester = std::move(*requester_result);
    ipc::IpcMessage input;
    make(&input, 10000, &destroyed, &host_loaded);
    const auto* backing = input.get<Batch>().values.data();
    const auto capacity = input.get<Batch>().values.capacity();
    const auto not_ready = requester.trySend(1, 17, input);
    check(!not_ready && not_ready.error() == ipc::EIpcError::NOT_READY);
    check(input.get<Batch>().values.data() == backing && input.get<Batch>().values.capacity() == capacity);
    auto responder_result = core()->createResponder("test.direct.dll");
    check(bool(responder_result));
    auto responder = std::move(*responder_result);
    check(bool(requester.trySend(1, 17, input)) && input.empty());
    ipc::ReceivedRequest received;
    check(responder.tryReceive(received) == ipc::EIpcReceive::RECEIVED);
    check(received.message().get<Batch>().values.data() == backing);
    ipc::IpcMessage response;
    make(&response, 32, &destroyed, &host_loaded);
    const auto* result_backing = response.get<Batch>().values.data();
    check(bool(responder.trySendResponse(received.requesterId(), received.correlation(), response)) && response.empty());
    received.reset();
    check(destroyed == 1); // Source is released; only the explicit response crosses back.
    ipc::IpcMessage late;
    std::uint64_t correlation = 0;
    check(requester.tryReceive(correlation, late) == ipc::EIpcReceive::RECEIVED);
    check(correlation == 17);
    requester.reset(); responder.reset();
    check(late.get<Batch>().values.data() == result_backing && late.get<Batch>().values[31] == 91);
    check(destroyed == 1 && GetModuleHandleW(L"render_client_dll_plugin.dll") != nullptr);
    late.reset();
    check(destroyed == 2);
    // Explicit host order: every message and deletion call stack has ended before unloading code.
    check(FreeLibrary(module) != 0);
    host_loaded = false;
    check(GetModuleHandleW(L"render_client_dll_plugin.dll") == nullptr);
    std::puts("Direct IPC DLL: one Core, cold declarations, moved backing, NotReady retention, host-ordered unload");
}
