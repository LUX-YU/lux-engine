#include "ClientDllFixture.hpp"
#include "../pinclude/TypedProtocol.hpp"
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <Windows.h>
#include <cstdio>
#include <optional>

using namespace lux::render;
namespace ipc = lux::platform::ipc;
namespace detail = lux::render::runtime_detail;
using client_dll_test::Batch;
using client_dll_test::Command;

static void check(bool value, const char* reason)
{
    if (!value)
    {
        std::fprintf(stderr, "Client DLL: %s\n", reason);
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
}

template <class T> static T take(RenderResult<T> result)
{
    check(bool(result), "unexpected Render error");
    return std::move(*result);
}

static ipc::ReceivedRequest receive(ipc::IpcResponder& responder)
{
    ipc::ReceivedRequest input;
    for (unsigned attempt = 0; attempt != 16; ++attempt)
        if (responder.tryReceive(input) == ipc::EIpcReceive::RECEIVED) return input;
    check(false, "request did not arrive");
    return input;
}

static RenderCompletion consume(RenderClient& client)
{
    RenderCompletion completion;
    check(take(client.tryReceive(completion)) == ERenderReceive::RECEIVED, "reply did not arrive");
    return completion;
}

static void publish(ipc::IpcResponder& responder, ipc::ReceivedRequest& input,
    const detail::CommandTransport& transport, void* response, SessionIdentity session,
    const std::shared_ptr<const void>& pin)
{
    auto& message = const_cast<ipc::IpcMessage&>(input.message());
    const auto metadata = message.metadata<detail::RequestMetadata>();
    const auto bytes = take(transport.response_bytes(response));
    check(bytes <= metadata.reply_capacity, "response exceeded its existing reservation");
    const auto overhead = transport.response->size() - transport.descriptor->response.size;
    auto lease = transport.request_budget(message).split(bytes + overhead);
    ipc::IpcMessage output;
    transport.package_response(output, response, std::move(lease), pin);
    check(output.operations() == transport.response, "reply did not use the canonical DLL TypeOps");
    output.setMetadata(detail::ReplyMetadata{session, metadata.request, ECommandOutcome::SUCCESS});
    check(bool(responder.tryReply(input, output)), "reply publication failed");
}

static void canonicalPlugin()
{
    bool pin_alive = true;
    int request_destroyed = 0;
    int response_destroyed = 0;
    const auto module = LoadLibraryW(L"render_client_dll_plugin.dll");
    check(module != nullptr, "plugin load failed");
    using Query = const detail::CommandTransport* (*)() noexcept;
    const auto query_command = reinterpret_cast<Query>(GetProcAddress(module, "clientDllCommand"));
    const auto query_hello = reinterpret_cast<Query>(GetProcAddress(module, "clientDllHello"));
    check(query_command && query_hello, "plugin exports missing");
    auto pin = std::shared_ptr<const void>(module, [&pin_alive](const void* handle) noexcept {
        pin_alive = false;
        check(FreeLibrary(static_cast<HMODULE>(const_cast<void*>(handle))) != 0, "plugin unload failed");
    });
    const auto* transport = query_command();
    const auto* hello_transport = query_hello();
    check(transport->request != detail::commandTransport<Command>().request,
        "fixture must have distinct EXE/DLL TypeOps addresses");

    auto responder_result = ipc::IpcCore::instance().createResponder("runtime.client.dll");
    check(bool(responder_result), "responder create failed");
    auto responder = std::move(*responder_result);
    auto client = take(RenderClient::create("runtime.client.dll"));
    const SessionIdentity session{43, 59};
    const auto hello_id = take(client->hello());
    auto hello_input = receive(responder);
    const auto* hello_actual = hello_input.message().operations();
    check(hello_actual && bool(ipc::IpcMessage::bindType<detail::ValueEnvelope<detail::HelloRequest>>(*hello_actual)),
        "cross-DLL Hello did not satisfy the cold type contract");
    detail::HelloResponse hello;
    const auto& descriptor = *transport->descriptor;
    hello.capabilities.push_back({pin, std::string(descriptor.name.name()), {0, 1}, descriptor.version,
        descriptor.request, descriptor.response, descriptor.completion, transport->request, transport->response});
    publish(responder, hello_input, *hello_transport, &hello, session, pin);
    auto greeting = consume(*client);
    check(greeting.header.request == hello_id && greeting.header.outcome == ECommandOutcome::SUCCESS,
        "cross-DLL owned capability manifest rejected");
    check(!client->bind<client_dll_test::WrongCommand>(), "same-name wrong Request ABI accepted");
    std::optional<BoundCommand<Command>> binding(take(client->bind<Command>()));
    greeting = {};
    check(client->retainedBytes() == 0, "Hello owner budget leaked");

    Batch request;
    request.identity = 73;
    request.values = {5, 7, 11};
    request.values.reserve(127);
    request.destroyed = &request_destroyed;
    request.pin_alive = &pin_alive;
    const auto* request_pointer = request.values.data();
    const auto request_capacity = request.values.capacity();
    const auto request_id = take(client->submit(*binding, request, 2048));
    auto input = receive(responder);
    check(input.message().operations() == transport->request,
        "bound Client did not construct the canonical DLL Request type");
    const auto& received = *static_cast<const Batch*>(transport->request_value(input.message()));
    check(received.identity == 73 && received.values.data() == request_pointer &&
        received.values.capacity() == request_capacity, "Request vector was copied across the DLL");
    Batch response;
    response.identity = 79;
    response.values = {13, 17};
    response.values.reserve(63);
    response.destroyed = &response_destroyed;
    response.pin_alive = &pin_alive;
    const auto* response_pointer = response.values.data();
    publish(responder, input, *transport, &response, session, pin);
    auto completion = consume(*client);
    std::optional<TypedRenderCompletion<Command>> late(take(std::move(completion).take(*binding)));
    check(late->header().request == request_id && late->response().identity == 79 &&
        late->response().values.data() == response_pointer && late->request().values.data() == request_pointer,
        "typed completion lost canonical owners");
    responder.reset();
    client.reset();
    binding.reset();
    pin.reset();
    check(pin_alive && request_destroyed == 0 && response_destroyed == 0,
        "Client or binding destruction released a late completion's module");
    check(late->response().values[1] == 17, "late Response is no longer accessible");
    late.reset();
    check(!pin_alive && request_destroyed == 1 && response_destroyed == 1,
        "final module release did not follow both DLL destruction thunks");
    check(GetModuleHandleW(L"render_client_dll_plugin.dll") == nullptr, "plugin remained loaded");
    std::printf("Client DLL: full cold ABI, canonical dispatch, vector identity and late CodePin PASS\n");
}

struct LocalCommand final
{
    static constexpr std::string_view name{"test.client.dll.local"};
    using Request = std::uint64_t;
    using Response = std::uint64_t;
};

static void clientImplementation()
{
    const auto module = LoadLibraryW(L"render_client_dll_plugin.dll");
    check(module != nullptr, "Client implementation plugin load failed");
    using Create = RenderClient* (*)(const char*) noexcept;
    const auto create = reinterpret_cast<Create>(GetProcAddress(module, "clientDllCreate"));
    check(create != nullptr, "Client factory export missing");
    auto responder_result = ipc::IpcCore::instance().createResponder("runtime.client.dll.implementation");
    check(bool(responder_result), "local responder create failed");
    auto responder = std::move(*responder_result);
    std::unique_ptr<RenderClient> client(create("runtime.client.dll.implementation"));
    check(bool(client), "plugin Client factory failed");
    const SessionIdentity session{83, 89};
    take(client->hello());
    auto hello_input = receive(responder);
    const auto* hello_actual = hello_input.message().operations();
    check(hello_actual && bool(ipc::IpcMessage::bindType<detail::ValueEnvelope<detail::HelloRequest>>(*hello_actual)),
        "plugin-created Client Hello did not satisfy the cold type contract");
    const auto local_transport = detail::commandTransport<LocalCommand>();
    const auto& descriptor = *local_transport.descriptor;
    detail::HelloResponse hello;
    hello.capabilities.push_back({{}, std::string(descriptor.name.name()), {0, 1}, descriptor.version,
        descriptor.request, descriptor.response, descriptor.completion,
        local_transport.request, local_transport.response});
    publish(responder, hello_input, detail::commandTransport<detail::Hello>(), &hello, session, {});
    auto greeting = consume(*client);
    check(greeting.header.outcome == ECommandOutcome::SUCCESS, "local canonical Hello failed");
    greeting = {};
    check(client->retainedBytes() == 0, "local Hello retained its budget");
    auto binding = take(client->bind<LocalCommand>());
    std::uint64_t request = 97;
    const auto request_id = take(client->submit(binding, request));
    auto input = receive(responder);
    check(input.message().operations() == local_transport.request, "canonical Request must reside in EXE");
    std::uint64_t response = 101;
    publish(responder, input, local_transport, &response, session, {});
    auto completion = consume(*client);
    std::optional<TypedRenderCompletion<LocalCommand>> late(take(std::move(completion).take(binding)));
    check(late->header().request == request_id && late->response() == 101 && client->retainedBytes() > 0,
        "late local completion did not own its budget lease");
    responder.reset();
    client.reset();
    check(FreeLibrary(module) != 0, "Client implementation plugin unload failed");
    check(GetModuleHandleW(L"render_client_dll_plugin.dll") == nullptr,
        "Client plugin must actually unload before budget destruction");
    check(late->response() == 101, "local canonical response died with Client plugin");
    std::puts("Client DLL: plugin unloaded; now releasing final late BudgetState control block");
    std::fflush(stdout);
    late.reset();
    std::puts("Client DLL: independently unloaded Client factory and late budget destruction PASS");
}

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    canonicalPlugin();
    clientImplementation();
}
