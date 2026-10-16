#include "ErasedRegistrationFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <cstdlib>
#include <chrono>
#include <thread>

using namespace lux::render;
static void check(bool value) { if (!value) std::abort(); }
struct First final
{
    static constexpr std::string_view name = "test.registration.first";
    using Request = std::uint32_t;
    using Response = Ack;
};
struct Second final
{
    static constexpr std::string_view name = "test.registration.second";
    using Request = std::uint32_t;
    using Response = Ack;
};
static RenderResult<void> describe(const RetainableEncodedDataView*, std::string_view, std::vector<ResourceClaim>&) noexcept
{
    return {};
}
static RenderResult<void> handler(void*, CommandInvocation&) noexcept { return {}; }

static RenderCompletion receive(RenderClient& client, RequestId request)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        RenderCompletion result;
        const auto status = client.tryReceive(result);
        check(bool(status));
        if (*status == ERenderReceive::RECEIVED)
        {
            check(result.header.request == request && result.header.outcome == ECommandOutcome::SUCCESS);
            return result;
        }
        check(*status != ERenderReceive::CLOSED);
        std::this_thread::yield();
    }
    std::abort();
}

static void featureDirectory()
{
    constexpr std::size_t count = 600; // Cross StableSlotMap blocks, retaining the original identities.
    std::vector<std::string> names;
    std::vector<FeatureDescriptor> descriptions;
    std::vector<FeatureId> ids;
    names.reserve(count);
    descriptions.reserve(count);
    ids.reserve(count);
    const FeatureGraphOps graph{describe, nullptr};
    auto created = RenderServer::create({"test.feature.directory", true});
    check(bool(created));
    auto server = std::move(*created);
    for (std::size_t i = 0; i < count; ++i)
    {
        names.push_back("test.feature." + std::to_string(i));
        descriptions.push_back({FeatureName(names.back()), {1, 0}, nullptr});
        auto registered = server->registerFeature({&descriptions.back(), &graph, nullptr});
        check(bool(registered));
        ids.push_back(*registered);
    }
    auto input = RenderClient::create("test.feature.directory");
    check(bool(input));
    auto client = std::move(*input);
    bool run_ok = false;
    std::thread owner([&] { run_ok = bool(server->run()); });
    auto hello = client->hello();
    check(bool(hello));
    receive(*client, *hello);
    auto binding = client->bind<ListFeatures>();
    check(bool(binding));
    EmptyRequest request;
    auto sent = client->submit(*binding, request, 128 * 1024);
    check(bool(sent));
    auto completed = receive(*client, *sent).take(*binding);
    check(bool(completed) && completed->response().features.size() == count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& value = completed->response().features[i];
        check(value.id == ids[i] && value.name == names[i]);
    }
    server->stop();
    owner.join();
    check(run_ok && server->stats().validation_errors == 0);
    server.reset();
    descriptions.clear();
    names.clear();
    check(completed->response().features.front().name == "test.feature.0");
}

int main()
{
    const FeatureDescriptor first{FeatureName("test.registration.a"), {1, 0}, nullptr};
    const FeatureDescriptor second{FeatureName("test.registration.b"), {1, 0}, nullptr};
    const FeatureGraphOps graph{describe, nullptr};
    // Assembly is independent of Commands; repeated instances remain legal.
    const FeatureInstanceView instances[]{{&first, &graph, nullptr, "one"}, {&first, &graph, nullptr, "two"}};
    check(bool(assembleFeatures(instances, {})));
    for (unsigned fault = 0; fault < 7; ++fault)
    {
        auto created = RenderServer::create({"test.feature.registration", false});
        check(bool(created));
        auto server = std::move(*created);
        auto original = commandDescriptor<First>();
        const std::array initial{test_registration::erased(&original, handler)};
        check(bool(server->registerFeature<First>({&first, &graph, nullptr, initial})));
        auto candidate = commandDescriptor<First>();
        if (fault == 1) ++candidate.version.minor;
        if (fault == 2) candidate.completion = ECompletionContract::RESOURCE_GPU_READY;
        if (fault == 3) candidate.response = candidate.request;
        if (fault >= 4) candidate = commandDescriptor<Second>();
        if (fault == 5) candidate.version = {};
        const std::array next{test_registration::erased(&candidate, handler)};
        if (fault < 4) check(!server->registerFeature<First>({&second, &graph, nullptr, next}));
        else if (fault == 4) check(bool(server->registerFeature<Second>({&second, &graph, nullptr, next})));
        else if (fault == 5) check(!server->registerFeature<Second>({&second, &graph, nullptr, next}));
        else
        {
            const std::array repeated{next[0], next[0]};
            check(!server->registerFeature<Second, Second>({&second, &graph, nullptr, repeated}));
        }
    }
    featureDirectory();
}
