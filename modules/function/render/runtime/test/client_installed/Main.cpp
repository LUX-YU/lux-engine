#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Native closure leaked into RenderClient
#endif

using namespace lux::render;

namespace
{
    struct ExternalCommand final
    {
        static constexpr std::string_view name{"installed.client.external"};
        struct Request final { std::uint32_t value = 0; };
        struct Response final { std::uint64_t value = 0; };
    };

    static_assert(__cplusplus >= 202002L);
    static_assert(RenderCommand<ExternalCommand>);
    static_assert(!std::is_default_constructible_v<BoundCommand<ExternalCommand>>);
    static_assert(!std::is_copy_constructible_v<TypedRenderCompletion<ExternalCommand>>);
    static_assert(requires(RenderClient& client, const BoundCommand<ExternalCommand>& binding,
        ExternalCommand::Request& request, RenderCompletion& completion) {
        { client.submit(binding, request) } -> std::same_as<RenderResult<RequestId>>;
        { std::move(completion).take(binding) } -> std::same_as<RenderResult<TypedRenderCompletion<ExternalCommand>>>;
    });

    void check(bool valid, int line) noexcept
    {
        if (valid) return;
        std::fprintf(stderr, "Pure Client consumer failed at line %d\n", line);
        std::_Exit(1);
    }
}

#define CHECK(value) check(static_cast<bool>(value), __LINE__)

int main()
{
    const auto invalid = RenderClient::create("installed.client.invalid", {0, 64});
    CHECK(!invalid && invalid.error().type == kRuntimeCapacity);
    auto made = RenderClient::create("installed.client.only.absent");
    CHECK(made);
    auto client = std::move(*made);
    CHECK(client->session() == SessionIdentity{});
    CHECK(client->retainedBytes() == 0);

    const auto external = client->bind<ExternalCommand>();
    const auto builtin = client->bind<QueryRenderStats>();
    CHECK(!external && external.error().type == kRuntimeState);
    CHECK(!builtin && builtin.error().type == kRuntimeState);

    // Genuine IPC NotReady must roll back the complete request reservation each time.
    for (unsigned attempt = 0; attempt < 4; ++attempt)
    {
        const auto hello = client->hello();
        CHECK(!hello && hello.error().type == kRuntimeState);
        CHECK(client->retainedBytes() == 0);
        RenderCompletion completion;
        const auto received = client->tryReceive(completion);
        CHECK(received && *received == ERenderReceive::EMPTY && !completion.active());
    }
    client.reset();
    std::puts("Pure relocated Client: Vulkan disabled, typed API and real NotReady rollback PASS. "
        "Successful Server request/reply is covered by the separate Runtime installed consumer.");
}
