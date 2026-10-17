#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Native closure leaked into the direct IPC Client
#endif

using namespace lux::render;
namespace ipc = lux::platform::ipc;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::abort(); } } while(false)

struct ExternalCommand final
{
    static constexpr std::string_view name = "installed.client.external";
    using Request = std::vector<unsigned>;
    using Response = unsigned;
};
static_assert(RenderCommand<ExternalCommand>);
static_assert(sizeof(RequestId) == sizeof(std::uint64_t));
static_assert(!std::is_copy_constructible_v<ipc::IpcMessage>);

int main()
{
    auto peer = ipc::IpcCore::instance().createRequester("installed.direct.client");
    CHECK(peer);
    ipc::IpcMessage message;
    auto& input = message.emplace<ExternalCommand::Request>(ExternalCommand::Request(4, 17));
    input.reserve(63);
    const auto* backing = input.data();
    const auto capacity = input.capacity();
    const RequestId request{1}; // Assigned and tracked by this application.
    message.setMetadata(RequestHeader{request, CommandId{1, 1}});
    for (unsigned i = 0; i < 4; ++i)
    {
        CHECK(!peer->trySend(message));
        CHECK(message.get<ExternalCommand::Request>().data() == backing);
        CHECK(message.get<ExternalCommand::Request>().capacity() == capacity);
    }
    auto service = ipc::IpcCore::instance().createResponder("installed.direct.client");
    CHECK(service && peer->trySend(message));
    ipc::ReceivedRequest accepted;
    CHECK(service->tryReceive(accepted) == ipc::EIpcReceive::RECEIVED);
    CHECK(accepted.message().get<ExternalCommand::Request>().data() == backing);
    const auto header = accepted.message().metadata<RequestHeader>();
    ipc::IpcMessage result;
    result.emplace<ExternalCommand::Response>(accepted.message().get<ExternalCommand::Request>()[0] + 1);
    result.setMetadata(ReplyHeader{header.request, ECommandOutcome::SUCCESS});
    CHECK(service->tryReply(accepted, result));
    CHECK(peer->tryReceive(message) == ipc::EIpcReceive::RECEIVED);
    CHECK(message.metadata<ReplyHeader>().request == request);
    CHECK(message.get<ExternalCommand::Response>() == 18);
    service->reset();
    peer->reset();
    CHECK(message.get<ExternalCommand::Response>() == 18); // Actual output outlives both roles.
    std::puts("Direct IPC + neutral Command declarations: NotReady preserves vector, RequestId echoed, late result valid");
}
