#include <cstdio>
#include <cstdlib>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <vector>

#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Native closure leaked into the direct IPC Client
#endif

using namespace lux::render;
namespace ipc = lux::platform::ipc;

#define CHECK(value)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(value))                                                                                                  \
        {                                                                                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #value);                                                   \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

struct ExternalCommand final
{
    static constexpr std::string_view name = "installed.client.external";
    using Request = std::vector<unsigned>;

    struct Response final
    {
        RenderError error{};
        unsigned value = 0;
    };
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
    for (unsigned i = 0; i < 4; ++i)
    {
        CHECK(!peer->trySend(commandTag(CommandId{1, 1}), request.value, message));
        CHECK(message.get<ExternalCommand::Request>().data() == backing);
        CHECK(message.get<ExternalCommand::Request>().capacity() == capacity);
    }
    auto service = ipc::IpcCore::instance().createResponder("installed.direct.client");
    CHECK(service && peer->trySend(commandTag(CommandId{1, 1}), request.value, message));
    ipc::ReceivedRequest accepted;
    CHECK(service->tryReceive(accepted) == ipc::EIpcReceive::RECEIVED);
    CHECK(accepted.message().get<ExternalCommand::Request>().data() == backing);
    ipc::IpcMessage result;
    auto& response = result.emplace<ExternalCommand::Response>();
    response.value = accepted.message().get<ExternalCommand::Request>()[0] + 1;
    CHECK(service->trySendResponse(accepted.requesterId(), accepted.correlation(), result));
    accepted.reset();
    std::uint64_t correlation = 0;
    CHECK(peer->tryReceive(correlation, message) == ipc::EIpcReceive::RECEIVED);
    CHECK(correlation == request.value);
    CHECK(message.get<ExternalCommand::Response>().error.type == 0);
    CHECK(message.get<ExternalCommand::Response>().value == 18);
    service->reset();
    peer->reset();
    CHECK(message.get<ExternalCommand::Response>().error.type == 0);
    CHECK(message.get<ExternalCommand::Response>().value == 18); // Actual output outlives both roles.
    std::puts(
        "Direct IPC + neutral Command declarations: NotReady preserves vector, RequestId echoed, late result valid"
    );
}
