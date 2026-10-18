#include "ServerFixture.hpp"
#include "Discovery.hpp"
#include "FeatureFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <cstdlib>
#include <source_location>
#include <cstdio>
#include <chrono>
#include <thread>

using namespace lux::render;
static void check(bool value, std::source_location at = std::source_location::current())
{
    if (!value)
    {
        std::fprintf(stderr, "CHECK %s:%u\n", at.file_name(), at.line());
        std::abort();
    }
}
struct First final
{
    static constexpr std::string_view name = "test.registration.first";
    using Request = std::uint32_t;
    using Response = RenderResult<Ack>;
};
struct Second final
{
    static constexpr std::string_view name = "test.registration.second";
    using Request = std::uint32_t;
    using Response = RenderResult<Ack>;
};
static RenderResult<void> describe(const void*, std::string_view, ResourceClaims&) noexcept
{
    return {};
}

struct InstallFeature final
{
    static constexpr std::string_view name = "test.feature.install";
    using Request = unsigned;
    using Response = RenderResult<FeatureId>;
};

struct DirectoryInstaller final
{
    static constexpr std::string_view name = "test.feature.installer";
    RenderServer& server;
    explicit DirectoryInstaller(RenderServer& value) noexcept : server(value) {}

    void install(const unsigned& number, InstallFeature::Response& reply) noexcept
    {
        const auto name = "test.feature." + std::to_string(number);
        const FeatureDescriptor descriptor{FeatureName(name), {}};
        const FeatureGraphOps graph{describe, nullptr};
        auto result = feature_test::registerGraph(server, {&descriptor, &graph, nullptr});
        reply = std::move(result);
    }

    using Bindings = std::tuple<CommandMethod<InstallFeature, &DirectoryInstaller::install>>;
};

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
    auto created = createTestServer({"test.feature.directory", true});
    check(bool(created));
    auto server = std::move(*created);
    check(bool(server->emplaceHandler<DirectoryInstaller>("", *server)));
    for (std::size_t i = 0; i < count; ++i)
    {
        names.push_back("test.feature." + std::to_string(i));
        descriptions.push_back({FeatureName(names.back()),
            i == 0 ? cppType<std::uint32_t>() : CppType{}});
        auto registered = feature_test::registerGraph(*server, {&descriptions.back(), &graph, nullptr});
        check(bool(registered));
        ids.push_back(*registered);
    }
    descriptions.clear();
    names.clear();
    auto input = runtime_test::connect("test.feature.directory", *server->commandId<ListCommands>());
    check(bool(input));
    auto client = std::move(*input);
    bool run_ok = false;
    std::thread owner([&] { run_ok = bool(server->run()); });
    check(bool(runtime_test::refresh(*client)));
    auto binding = runtime_test::find<ListFeatures>(*client);
    check(bool(binding));
    EmptyRequest request;
    auto sent = runtime_test::send<ListFeatures>(*client, *binding, request);
    check(bool(sent));
    auto completed = runtime_test::result<ListFeatures>(runtime_test::wait(*client, *sent));
    check(bool(completed) && completed->features.size() == count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& value = completed->features[i];
        check(value.id == ids[i] && value.name == "test.feature." + std::to_string(i));
        if (i == 0)
            check(value.config_type == lux::cxx::typeToken<std::uint32_t>().name() &&
                value.config_size == sizeof(std::uint32_t) && value.config_alignment == alignof(std::uint32_t));
        else
            check(value.config_type.empty() && value.config_size == 0 && value.config_alignment == 0);
    }
    const auto installing = *runtime_test::find<InstallFeature>(*client);
    unsigned number = count;
    auto added = runtime_test::coldCall<InstallFeature>(*client, number);
    check(bool(added) && !added->isNull());
    auto repeated = runtime_test::coldCall<InstallFeature>(*client, number);
    check(repeated && *repeated == *added);
    auto updated = runtime_test::coldCall<ListFeatures>(*client, request);
    check(updated && updated->features.size() == count + 1);
    check(updated->features.front().id == ids.front());
    check(updated->features.back().id == *added);
    check(updated->features.back().name == "test.feature.600");
    server->stop();
    owner.join();
    check(run_ok && server->stats().validation_errors == 0);
    server.reset();
    descriptions.clear();
    names.clear();
    check(completed->features.front().name == "test.feature.0");
    check(completed->features.front().config_type == lux::cxx::typeToken<std::uint32_t>().name());

    // A persisted preset saves the stable name, never the Server-local FeatureId. Reversing the
    // install order on a new Server makes stale numeric reuse visible instead of accidentally passing.
    const std::string preset = completed->features.front().name;
    auto restarted = createTestServer({"test.feature.directory", true});
    check(bool(restarted));
    auto next = std::move(*restarted);
    const FeatureDescriptor other{FeatureName("test.feature.restart-first"), {}};
    const FeatureDescriptor restored{FeatureName(preset), cppType<std::uint32_t>()};
    check(bool(feature_test::registerGraph(*next, {&other, &graph, nullptr})));
    const auto restored_id = feature_test::registerGraph(*next, {&restored, &graph, nullptr});
    check(restored_id && *restored_id != ids.front());
    auto connected = runtime_test::connect("test.feature.directory", *next->commandId<ListCommands>());
    check(bool(connected));
    auto next_client = std::move(*connected);
    std::thread next_owner([&] { run_ok = bool(next->run()); });
    // A new connection discovers its own directory. Old numeric CommandIds carry no cross-link proof.
    check(bool(runtime_test::refresh(*next_client)));
    auto next_features = runtime_test::coldCall<ListFeatures>(*next_client, {});
    check(bool(next_features));
    const auto found = std::ranges::find(next_features->features, preset, &FeatureInfo::name);
    check(found != next_features->features.end() && found->id == *restored_id);
    next->stop();
    next_owner.join();
    check(run_ok && next->stats().validation_errors == 0);
}

struct alignas(16) AlignedRequest final { std::uint32_t value; };

template <unsigned Fault> struct TestedHandler final
{
    static constexpr std::string_view name = "test.feature.registration.handler";
    struct Command final
    {
        static constexpr std::string_view name = Fault == 5 ? "" : Fault == 4 ? Second::name : First::name;
        using Request = std::conditional_t<Fault == 1, std::uint64_t,
            std::conditional_t<Fault == 2, AlignedRequest, std::uint32_t>>;
        using Response = RenderResult<std::conditional_t<Fault == 3, std::uint32_t, Ack>>;
    };

    void run(const typename Command::Request&, typename Command::Response& reply) noexcept
    {
        reply.emplace();
    }

    using Binding = CommandMethod<Command, &TestedHandler::run>;
    using Bindings = std::conditional_t<Fault == 6, std::tuple<Binding, Binding>, std::tuple<Binding>>;
};

template <unsigned Fault> static void registrationFault()
{
    const FeatureDescriptor first{FeatureName("test.registration.a"), {}};
    const FeatureDescriptor second{FeatureName("test.registration.b"), {}};
    const FeatureGraphOps graph{describe, nullptr};
    auto created = createTestServer({"test.feature.registration", false});
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
