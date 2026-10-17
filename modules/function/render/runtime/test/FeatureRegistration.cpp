#include "Discovery.hpp"
#include "FeatureFixture.hpp"
#include "ProtocolAudit.hpp"
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
static RenderResult<void> describe(const void*, std::string_view, ResourceClaims&) noexcept
{
    return {};
}
static RenderResult<void> handler(void*, CommandInvocation&) noexcept { return {}; }

struct InstallFeature final
{
    static constexpr std::string_view name = "test.feature.install";
    using Request = unsigned;
    using Response = FeatureId;
};

struct DirectoryInstaller final
{
    static constexpr std::string_view name = "test.feature.installer";
    RenderServer& server;
    explicit DirectoryInstaller(RenderServer& value) noexcept : server(value) {}

    RenderResult<void> install(const unsigned& number, Reply<InstallFeature>& reply) noexcept
    {
        const auto name = "test.feature." + std::to_string(number);
        const FeatureDescriptor descriptor{FeatureName(name), {}};
        const FeatureGraphOps graph{describe, nullptr};
        auto result = feature_test::registerGraph(server, {&descriptor, &graph, nullptr});
        return result ? reply.complete(*result) : reply.fail(result.error());
    }

    using Bindings = std::tuple<CommandMethod<InstallFeature, &DirectoryInstaller::install>>;
};

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
    check(bool(server->emplaceHandler<DirectoryInstaller>("", {}, *server)));
    for (std::size_t i = 0; i < count; ++i)
    {
        names.push_back("test.feature." + std::to_string(i));
        descriptions.push_back({FeatureName(names.back()),
            i == 0 ? ConfigValue::typeOf<std::uint32_t>() : ConfigValue::Type{}});
        auto registered = feature_test::registerGraph(*server, {&descriptions.back(), &graph, nullptr});
        check(bool(registered));
        ids.push_back(*registered);
    }
    descriptions.clear();
    names.clear();
    auto input = RenderClient::create("test.feature.directory");
    check(bool(input));
    auto client = std::move(*input);
    bool run_ok = false;
    std::thread owner([&] { run_ok = bool(server->run()); });
    auto hello = client->hello();
    check(bool(hello));
    receive(*client, *hello);
    check(bool(runtime_test::refresh(*client)));
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
        check(value.id == ids[i] && value.name == "test.feature." + std::to_string(i));
        if (i == 0)
            check(value.config_type == lux::cxx::typeToken<std::uint32_t>().name() &&
                value.config_size == sizeof(std::uint32_t) && value.config_alignment == alignof(std::uint32_t));
        else
            check(value.config_type.empty() && value.config_size == 0 && value.config_alignment == 0);
    }
    const auto installing = *client->bind<InstallFeature>();
    unsigned number = count;
    auto added = receive(*client, *client->submit(installing, number)).take(installing);
    check(bool(added) && !added->response().isNull());
    auto repeated = receive(*client, *client->submit(installing, number)).take(installing);
    check(repeated && repeated->response() == added->response());
    auto updated = receive(*client, *client->submit(*binding, request, 128 * 1024)).take(*binding);
    check(updated && updated->response().features.size() == count + 1);
    check(updated->response().features.front().id == ids.front());
    check(updated->response().features.back().id == added->response());
    check(updated->response().features.back().name == "test.feature.600");
    server->stop();
    owner.join();
    check(run_ok && server->stats().validation_errors == 0);
    server.reset();
    descriptions.clear();
    names.clear();
    check(completed->response().features.front().name == "test.feature.0");
    check(completed->response().features.front().config_type == lux::cxx::typeToken<std::uint32_t>().name());

    // A persisted preset saves the stable name, never the Server-local FeatureId. Reversing the
    // install order on a new Server makes stale numeric reuse visible instead of accidentally passing.
    const std::string preset = completed->response().features.front().name;
    auto restarted = RenderServer::create({"test.feature.directory", true});
    check(bool(restarted));
    auto next = std::move(*restarted);
    const FeatureDescriptor other{FeatureName("test.feature.restart-first"), {}};
    const FeatureDescriptor restored{FeatureName(preset), ConfigValue::typeOf<std::uint32_t>()};
    check(bool(feature_test::registerGraph(*next, {&other, &graph, nullptr})));
    const auto restored_id = feature_test::registerGraph(*next, {&restored, &graph, nullptr});
    check(restored_id && *restored_id != ids.front());
    auto connected = RenderClient::create("test.feature.directory");
    check(bool(connected));
    auto next_client = std::move(*connected);
    std::thread next_owner([&] { run_ok = bool(next->run()); });
    auto next_hello = next_client->hello();
    check(bool(next_hello));
    receive(*next_client, *next_hello);
    auto stale_binding = next_client->submit(*binding, request, 128 * 1024);
    check(!stale_binding && stale_binding.error().type == kRuntimeIdentity);
    const auto next_binding = next_client->bind<ListFeatures>();
    check(bool(next_binding));
    auto next_sent = next_client->submit(*next_binding, request, 128 * 1024);
    check(bool(next_sent));
    auto next_features = receive(*next_client, *next_sent).take(*next_binding);
    check(bool(next_features));
    const auto found = std::ranges::find(next_features->response().features, preset, &FeatureInfo::name);
    check(found != next_features->response().features.end() && found->id == *restored_id);
    next->stop();
    next_owner.join();
    check(run_ok && next->stats().validation_errors == 0);
}

template <unsigned Fault> struct TestedHandler final
{
    static constexpr std::string_view name = "test.feature.registration.handler";
    struct Binding final
    {
        using Command = std::conditional_t<(Fault >= 4), Second, First>;
        static CommandHandler bind(TestedHandler& owner) noexcept
        {
            static const auto descriptor = [] {
                auto result = commandDescriptor<Command>();
                if constexpr (Fault == 1) result.name = CommandName("test.registration.wrong-name");
                if constexpr (Fault == 2) result.request.alignment *= 2;
                if constexpr (Fault == 3) result.response = result.request;
                if constexpr (Fault == 5) result.name = {};
                return result;
            }();
            return protocol_detail::CommandHandlerAudit::make(&descriptor, handler, &owner);
        }
    };
    using Bindings = std::conditional_t<Fault == 6, std::tuple<Binding, Binding>, std::tuple<Binding>>;
};

template <unsigned Fault> static void registrationFault()
{
    const FeatureDescriptor first{FeatureName("test.registration.a"), {}};
    const FeatureDescriptor second{FeatureName("test.registration.b"), {}};
    const FeatureGraphOps graph{describe, nullptr};
    auto created = RenderServer::create({"test.feature.registration", false});
    check(bool(created));
    auto server = std::move(*created);
    auto installed = server->registerFeature<First>(
        feature_test::Provider<TestedHandler<0>>{{&first, &graph, nullptr}});
    check(bool(installed));
    auto repeated = server->registerFeature<First>(
        feature_test::Provider<TestedHandler<0>>{{&first, &graph, nullptr}});
    check(repeated && *repeated == *installed);
    auto candidate = server->registerFeature<First>(
        feature_test::Provider<TestedHandler<Fault>>{{&second, &graph, nullptr}});
    if constexpr (Fault == 0 || Fault == 4) check(bool(candidate));
    else check(!candidate);
    // Reusing the same Handler across two Features is now valid; duplicate member entries are not.
    server->stop();
    check(!feature_test::registerGraph(*server, {&first, &graph, nullptr}));
}

int main()
{
    const FeatureDescriptor first{FeatureName("test.registration.a"), {}};
    const FeatureGraphOps graph{describe, nullptr};
    const FeatureInstanceView instances[]{{&first, &graph, nullptr, "one"}, {&first, &graph, nullptr, "two"}};
    check(bool(assembleFeatures(instances, {})));
    []<std::size_t... I>(std::index_sequence<I...>) { (registrationFault<I>(), ...); }(std::make_index_sequence<7>{});
    featureDirectory();
}
