#include "ServerFixture.hpp"
#include "FeatureFixture.hpp"
#include "CompletionTest.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include "TextureSceneFixture.hpp"
#include <lux/engine/render/native/vulkan/texture/TextureCommandHandler.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <cstdio>
#include <thread>
#include <latch>

using namespace lux::render;
using namespace std::chrono_literals;
static std::atomic<unsigned> mode{0};
static std::latch entered(1), resume(1);
static void check(bool value, std::source_location at = std::source_location::current())
{
    if (!value)
    {
        std::fprintf(stderr, "CHECK %s:%u\n", at.file_name(), at.line());
        std::abort();
    }
}
template <class T> static T take(RenderResult<T> value) { check(bool(value)); return std::move(*value); }

static TestCompletion call(TestClient& client, const TestCommand& command)
{
    const auto request = take(client.submit(command));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto result = take(receiveExpected(client, request));
        if (result) return std::move(*result);
        std::this_thread::sleep_for(1ms);
    }
    std::abort();
}

static RenderResult<ProgramContribution> ownPrograms(const void* wire, std::string_view name) noexcept
{
    if (mode == 4) { entered.count_down(); resume.wait(); }
    const auto* config = static_cast<const texture_fixture::Config*>(wire);
    auto values = texture_fixture::programs(*config, name);
    if (mode == 1) values.clear(); // foreign Feature tries to supply these instead
    if (mode == 2) values.push_back(values.front());
    if (mode == 3) values.pop_back(); // live Pass lacks a program
    auto arguments = texture_fixture::arguments(*config);
    if (mode == 7) arguments.front().resource_key = "unknown.argument";
    return ProgramContribution{std::move(values), std::move(arguments)};
}

static RenderResult<ProgramContribution> foreignPrograms(const void*, std::string_view) noexcept
{
    if (mode == 1) return ProgramContribution{texture_fixture::programs({}, "main"), {}};
    ProgramContribution result{{}, {{"shared:texture", *ConfigValue::make(TextureBinding{{}, mode == 5 ? 1u : 0u})}}};
    if (mode == 8)
    {
        PassProgram dead{featureKey(FeatureName("test.aaa.foreign"), "foreign", "unused")};
        dead.shader.interface.bindings = {{99, 99, EDescriptorKind::STORAGE_BUFFER, 99, ShaderBlock{99}}};
        dead.shader.stages = {{lux::rdesc::EShaderType::VERTEX, "main", {0}}}; // Pruned by the single G4 analysis.
        result.programs.push_back(std::move(dead));
    }
    return result;
}

static std::atomic<unsigned> parameter_calls{0};
static RenderResult<ProgramContribution> zeroPassParameters(const void* input, std::string_view) noexcept
{
    ++parameter_calls;
    auto value = *static_cast<const TextureBinding*>(input);
    check(value.lod == 0);
    if (mode == 6) value.lod = 1;
    return ProgramContribution{{}, {{"shared:texture", *ConfigValue::make(value)}}};
}

static RenderResult<void> emptyDescribe(const void*, std::string_view, ResourceClaims&) noexcept
{
    return {};
}

int main()
{
    const FeatureDescriptor foreign{FeatureName("test.aaa.foreign"), {}};
    const FeatureGraphOps foreign_graph{emptyDescribe, [](const void*, std::string_view name, std::span<const ResolvedResource>, RenderGraphBuilder& builder) noexcept
            -> RenderResult<void> {
            auto pass = builder.addPass({featureKey(FeatureName("test.aaa.foreign"), name, "unused"),
                EGraphPassKind::TRANSFER, {}});
            return pass ? RenderResult<void>{} : lux::cxx::unexpected(pass.error());
        }};
    const FeatureDescriptor parameters{FeatureName("test.zzz.parameters"), cppType<TextureBinding>()};
    const FeatureGraphOps parameter_graph{emptyDescribe, nullptr};
    const FeatureDescriptor dead{FeatureName("test.dead.no.program"), {}};
    const FeatureGraphOps dead_graph{emptyDescribe,
        [](const void*, std::string_view name, std::span<const ResolvedResource>, RenderGraphBuilder& builder) noexcept
            -> RenderResult<void> {
            auto pass = builder.addPass({featureKey(FeatureName("test.dead.no.program"), name, "unused"),
                EGraphPassKind::TRANSFER, {}});
            return pass ? RenderResult<void>{} : lux::cxx::unexpected(pass.error());
        }};
    RenderServerConfig server_config{"runtime.program.ownership", true};
    vulkan::DeviceOptions server_config_device{.dynamic_rendering = true};
    server_config_device.texture_descriptors = true;
    auto server = take(createTestServer(server_config, server_config_device));
    check(bool(server->registerStorage<TextureStorage>()));
    check(bool(feature_test::registerGraph(*server, {&dead, &dead_graph, nullptr})));
    check(bool(feature_test::registerGraph(*server, {&parameters, &parameter_graph, zeroPassParameters})));
    check(bool(feature_test::registerGraph(*server, {&texture_fixture::definition, &texture_fixture::definition_graph, ownPrograms})));
    check(bool(feature_test::registerGraph(*server, {&foreign, &foreign_graph, foreignPrograms})));
    auto client = take(TestClient::create("runtime.program.ownership", take(server->commandId<ListCommands>())));
    bool run_ok = false;
    std::thread worker([&] { run_ok = bool(server->run()); });
    TestCommand command;
    (void)call(*client, command);
    command.operation = ETestCommand::CREATE_EFFECT;
    command.description = texture_fixture::target({});
    command.features.push_back({std::string(texture_fixture::type.name()), "main", take(ConfigValue::make(texture_fixture::Config{}))});
    command.features.push_back({"test.aaa.foreign", "foreign", {}});
    command.features.push_back({"test.zzz.parameters", "parameters", take(ConfigValue::make(TextureBinding{}))});
    command.features.push_back({"test.dead.no.program", "dead", {}});
    // Input order differs from canonical assembly order; provenance is the input index, not a name guess.
    check(call(*client, command).header.outcome == ETestOutcome::SUCCESS);
    check(parameter_calls == 1); // Zero Pass still contributes typed parameters, with no DataSchema.
    const std::array expected{kFeatureOwnership, kFeatureDuplicate, kFeatureContract};
    for (unsigned i = 1; i <= 3; ++i)
    {
        mode = i;
        auto failed = call(*client, command);
        check(failed.header.outcome == ETestOutcome::FAILED && failed.error.type == expected[i - 1]);
        check(server->stats().compiled_graphs == 1);
    }
    mode = 0;
    check(call(*client, command).header.outcome == ETestOutcome::SUCCESS);
    check(server->stats().compiled_graphs == 2);
    mode = 5;
    auto conflict = call(*client, command);
    check(conflict.header.outcome == ETestOutcome::FAILED && conflict.error.type == kStorageMismatch);
    check(server->stats().compiled_graphs == 2);
    mode = 6;
    auto zero_pass_conflict = call(*client, command);
    check(zero_pass_conflict.header.outcome == ETestOutcome::FAILED &&
        zero_pass_conflict.error.type == kStorageMismatch && server->stats().compiled_graphs == 2);
    mode = 7;
    auto unknown_argument = call(*client, command);
    check(unknown_argument.header.outcome == ETestOutcome::FAILED &&
        unknown_argument.error.type == kStorageContract && server->stats().compiled_graphs == 2);
    mode = 8;
    check(call(*client, command).header.outcome == ETestOutcome::SUCCESS);
    check(server->stats().compiled_graphs == 3);
    mode = 4;
    take(client->submit(command));
    entered.wait();
    client.reset(); // candidate still owns its input while the caller disappears
    resume.count_down();
    const auto ticks = server->stats().ticks;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (server->stats().ticks < ticks + 100 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    check(server->stats().ticks >= ticks + 100 && server->stats().compiled_graphs == 4); // Accepted work commits even after its requester closes.
    server->stop();
    server->stop();
    worker.join();
    check(run_ok && server->stats().validation_errors == 0);
    auto missing = take(createTestServer({"runtime.no.program", true}));
    check(bool(feature_test::registerGraph(*missing, {&texture_fixture::definition, &texture_fixture::definition_graph, nullptr})));
    auto no_program = take(TestClient::create("runtime.no.program", take(missing->commandId<ListCommands>())));
    bool missing_run_ok = false;
    std::thread missing_worker([&] { missing_run_ok = bool(missing->run()); });
    TestCommand missing_command;
    (void)call(*no_program, missing_command);
    missing_command.operation = ETestCommand::CREATE_EFFECT;
    missing_command.description = texture_fixture::target({});
    missing_command.features.push_back(command.features.front());
    auto missing_reply = call(*no_program, missing_command);
    check(missing_reply.header.outcome == ETestOutcome::FAILED &&
        missing_reply.error.type == kFeatureContract && missing->stats().compiled_graphs == 0);
    missing->stop();
    missing_worker.join();
    check(missing_run_ok && missing->stats().validation_errors == 0);
    auto stopped = take(createTestServer({"runtime.stopped.before.run", true}));
    stopped->stop();
    stopped->stop();
    check(bool(stopped->run()) && !stopped->run());
    check(stopped->stats().ticks == 0 && stopped->stats().submissions == 0);
    std::puts("Program contribution: foreign/duplicate/missing rejected; canonical provenance and last-good preserved");
}
