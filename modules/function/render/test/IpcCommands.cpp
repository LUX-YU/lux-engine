#include <atomic>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/resources/mesh/Commands.hpp>
#include <lux/engine/render/resources/instance/Commands.hpp>
#include <lux/engine/render/resources/instance/Data.hpp>
#include <type_traits>

#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Native closure leaked into the Render IPC command fixture
#endif

#include "TestSupport.hpp"

static_assert(std::is_trivially_copyable_v<lux::render::TransformUpdate>);
static_assert(std::is_standard_layout_v<lux::render::TransformUpdate>);
static_assert(std::is_trivially_copyable_v<lux::render::MeshId> && std::is_standard_layout_v<lux::render::MeshId>);
static_assert(sizeof(lux::render::MeshId) == 16);
static_assert(sizeof(lux::render::RequestId) == sizeof(std::uint64_t));
static_assert(!std::is_copy_constructible_v<lux::platform::ipc::IpcMessage>);

struct Scalar final
{
    std::uint64_t value;
};

struct ScalarResponse final
{
    lux::render::RenderError error{};
    std::uint64_t value = 0;
};

struct Increment final
{
    static constexpr std::string_view name = "ipc.increment";
    using Request = Scalar;
    using Response = ScalarResponse;
};

struct AssetRequest final
{
    Scalar scalar;
    lux::cxx::SharedBytes<> asset;
};

struct AssetIncrement final
{
    static constexpr std::string_view name = "ipc.asset.increment";
    using Request = AssetRequest;
    using Response = ScalarResponse;
};

namespace
{
    using namespace ipc_test;

    void smallCommandAllocations()
    {
        using namespace lux::render;
        const auto& descriptor = commandDescriptor<Increment>();
        check(static_cast<bool>(compatibleCommands(descriptor, descriptor)), "cold full typed command binding");
        lux::cxx::SlotMap<CommandDescriptor, CommandTag> routes;
        const auto command = routes.emplace(descriptor);
        auto& core = IpcCore::instance();
        auto server = must(core.createResponder("command.allocations"));
        auto client = must(core.createRequester("command.allocations"));
        std::uint64_t next = 1; // This caller owns its correlation sequence.
        auto execute = [&](ReceivedRequest& work)
        {
            const auto command_id = commandIdFromTag(work.tag());
            check(routes.find(command_id) != nullptr, "endpoint-local command dispatch");
            const auto& value = work.message().get<Scalar>();
            IpcMessage reply;
            reply.emplace<ScalarResponse>(ScalarResponse{{}, value.value + 1});
            publish(server, work, reply);
        };
        for (bool paired : {false, true})
        {
            std::atomic<bool> finish{false};
            std::atomic<std::size_t> worker_allocations{0};
            std::jthread receiver;
            if (paired)
            {
                receiver = std::jthread(
                    [&]
                    {
                        ipc_allocation::start();
                        testing::startAllocations();
                        while (!finish)
                        {
                            ReceivedRequest work;
                            if (server.tryReceive(work) == EIpcReceive::RECEIVED)
                                execute(work);
                        }
                        worker_allocations = ipc_allocation::stop() + testing::stopAllocations();
                    }
                );
            }
            for (std::uint64_t i = 0; i < 128; ++i)
            {
                ipc_allocation::start();
                testing::startAllocations();
                const RequestId id{next++};
                IpcMessage input;
                input.emplace<Scalar>(Scalar{i});
                must(client.trySend(commandTag(command), id.value, input));
                if (!paired)
                {
                    auto work = receive(server);
                    execute(work);
                }
                IpcMessage reply;
                std::uint64_t correlation = 0;
                const auto deadline = Clock::now() + 10s;
                while (client.tryReceive(correlation, reply) != EIpcReceive::RECEIVED)
                {
                    check(Clock::now() < deadline, "typed command must complete");
                    std::this_thread::yield();
                }
                check(
                    correlation == id.value && reply.get<ScalarResponse>().error.type == 0 &&
                        reply.get<ScalarResponse>().value == i + 1,
                    "validate typed terminal correlation and success"
                );
                const auto exe = ipc_allocation::stop(), dll = testing::stopAllocations();
                check(exe == 0 && dll == 0, "complete small typed command zero allocations");
            }
            finish = true;
            if (receiver.joinable())
            {
                receiver.join();
            }
            check(worker_allocations == 0, "server typed command path zero allocations");
        }
        client.reset();
        server.reset();
        empty();
    }

    void protocol()
    {
        using namespace lux::render;
        const auto& descriptor = commandDescriptor<AssetIncrement>();
        check(static_cast<bool>(validateCommand(descriptor)), "typed command contract");
        auto conflict = descriptor;
        conflict.response = lux::cxx::make_basic_type_info<Ack>();
        check(!compatibleCommands(descriptor, conflict), "response type rejected by pure protocol");
        conflict = descriptor;
        conflict.requires_idle_resource = !descriptor.requires_idle_resource;
        check(!compatibleCommands(descriptor, conflict), "Resource access policy is part of the cold contract");
        conflict = descriptor;

        struct alignas(64) AlignedRequest
        {
            std::uint64_t value;
        };

        conflict.request = lux::cxx::make_basic_type_info<AlignedRequest>();
        check(!compatibleCommands(descriptor, conflict), "different aligned request type rejected");
        conflict.request = {};
        check(!validateCommand(conflict), "missing request type rejected");
        conflict = descriptor;
        conflict.request = lux::cxx::basic_type_info(conflict.request.hash(), "collision.other");
        check(!compatibleCommands(descriptor, conflict), "full type name rejects a hash collision");

        auto& core = IpcCore::instance();
        auto a = must(core.createRequester("protocol"));
        const RequestId id_a{1};
        const RequestId id_b{1};
        check(id_a == id_b, "real Render RequestId collisions across independent clients");
        const CommandId command{0, 1};

        int shared_destroyed = 0;
        auto shared_source = std::make_shared<Owner>(1024 * 1024, &shared_destroyed);
        std::weak_ptr<Owner> weak_source = shared_source;
        auto source = lux::cxx::SharedBytes<>::fromOwner(shared_source, std::span(shared_source->bytes).first(8));
        const auto* pointer = source.data();
        IpcMessage input;
        input.emplace<AssetRequest>(AssetRequest{{5}, std::move(source)});
        shared_source.reset();
        check(shared_destroyed == 0, "typed request pins original shared asset");
        const auto unaccepted = a.trySend(commandTag(command), id_a.value, input);
        check(!unaccepted && unaccepted.error() == EIpcError::NOT_READY, "requester can precede responder");
        check(input.get<AssetRequest>().asset.data() == pointer, "unaccepted command preserves the original asset");
        auto server = must(core.createResponder("protocol"));
        auto b = must(core.createRequester("protocol"));
        must(a.trySend(commandTag(command), id_a.value, input));
        IpcMessage other;
        other.emplace<AssetRequest>(AssetRequest{{101}, {}});
        must(b.trySend(commandTag(command), id_b.value, other));
        auto first = receive(server);
        auto second = receive(server);
        auto* asset = first.message().get<AssetRequest>().asset.data() == pointer ? &first : &second;
        auto* other_work = asset == &first ? &second : &first;
        check(asset->message().get<AssetRequest>().asset.data() == pointer, "typed asset transferred without copy");
        check(!weak_source.expired(), "source retained after original handle destroyed");
        const auto answer = [&](ReceivedRequest& work)
        {
            const auto command_id = commandIdFromTag(work.tag());
            const auto& request = work.message().get<AssetRequest>();
            IpcMessage reply;
            reply.emplace<ScalarResponse>(ScalarResponse{{}, request.scalar.value + 1});
            publish(server, work, reply);
        };
        answer(*other_work);
        answer(*asset);
        {
            std::uint64_t correlation_a = 0, correlation_b = 0;
            auto result_a = receivedReply(a, &correlation_a);
            auto result_b = receivedReply(b, &correlation_b);
            check(
                correlation_a == id_a.value && result_a.get<ScalarResponse>().error.type == 0 &&
                    result_a.get<ScalarResponse>().value == 6,
                "reply A despite equal Render ID"
            );
            check(
                correlation_b == id_b.value && result_b.get<ScalarResponse>().error.type == 0 &&
                    result_b.get<ScalarResponse>().value == 102,
                "reply B despite reverse completion"
            );
            check(shared_destroyed == 1, "input released when business finishes, not returned implicitly");
        }
        check(shared_destroyed == 1 && weak_source.expired(), "shared asset last release exactly once");

        // A wrong get<T>() is a caller precondition violation, not runtime ABI negotiation.
        // Invalid command identity remains a protocol boundary rejection without touching its payload.
        IpcMessage wrong;
        wrong.emplace<Scalar>(Scalar{7});
        must(a.trySend(commandTag(CommandId{}), id_a.value, wrong));
        auto rejected = receive(server);
        check(commandIdFromTag(rejected.tag()) == CommandId{}, "invalid command can be rejected before access");
        rejected.reset();
        IpcMessage unanswered;
        check(receiveReply(a, unanswered) == EIpcReceive::EMPTY, "IPC does not invent a business outcome");
        a.reset();
        b.reset();
        server.reset();
        empty();
    }

} // namespace

int main()
{
    protocol();
    smallCommandAllocations();
}
