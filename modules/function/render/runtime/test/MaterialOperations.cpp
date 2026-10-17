#include "../../native/vulkan/test/Support.hpp"
#include "Discovery.hpp"
#include "ServerFixture.hpp"
#include "TypedTest.hpp"
#include <lux/engine/render/native/vulkan/material/MaterialCommands.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/resource/Material.hpp>
#include <fstream>

using namespace foundation_test;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;



int main(int argc, char** argv)
{
    CHECK(argc == 2);
    std::ifstream source(argv[1], std::ios::binary | std::ios::ate);
    CHECK(source);
    const auto bytes = source.tellg();
    CHECK(bytes > 0 && bytes % 4 == 0);
    MaterialProgram program;
    program.fragment = {lux::rdesc::EShaderType::FRAGMENT, "main", std::vector<std::uint32_t>(std::size_t(bytes) / 4)};
    source.seekg(0);
    CHECK(source.read(reinterpret_cast<char*>(program.fragment.words.data()), bytes));
    program.parameter_count = 1;
    program.defaults[0] = {.25f, .25f, .25f, 1};
    auto cooked = take(ConfigValue::make(std::move(program)));
    const auto* original_words = cooked.borrow<MaterialProgram>()->fragment.words.data();
    CpuFixture host;
    RenderServerConfig config{"runtime.material.operations", true};
    vulkan::DeviceOptions config_device{.dynamic_rendering = true};
    config_device.multiple_queues = true;
    config.uploads = TransferOptions{65536, 2, 131072};
    auto server = take(createTestServer(host, config, config_device));
    const auto type = take(server->registerStorage<lux::render::MaterialStorage>());
    CHECK(take(server->registerStorage<lux::render::MaterialStorage>()) == type);
    bool run_ok = false;
    const auto directory_id = take(server->commandId<ListCommands>());
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto a = take(runtime_test::connect(config.channel_name, directory_id));
    auto b = take(runtime_test::connect(config.channel_name, directory_id));
    CHECK(runtime_test::refresh(*a) && runtime_test::refresh(*b));
    const auto storage = take(runtime_test::prepareStorage(*a, kMaterialStorageType.name(), "material"));
    CHECK(take(runtime_test::prepareStorage(*b, kMaterialStorageType.name(), "material")) == storage);
    const auto set_a = take(runtime_test::find<SetMaterial>(*a)), set_b = take(runtime_test::find<SetMaterial>(*b));
    const auto query = take(runtime_test::find<QueryMaterial>(*b));
    const auto release = take(runtime_test::find<ReleaseMaterial>(*b));
    MaterialWrite input{{storage, {}}, 7, cooked, cooked.borrow<MaterialProgram>()->defaults};
    {
        auto failed = call<SetMaterial>(*a, set_a, input);
        CHECK(failed.metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED);
    }
    auto other = input;
    CHECK(call<SetMaterial>(*b, set_b, std::move(other)).metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED);
    input.expected_revision = 0;
    other = input;
    const auto id_a = take(runtime_test::send<SetMaterial>(*a, set_a, input));
    const auto id_b = take(runtime_test::send<SetMaterial>(*b, set_b, other));
    CHECK(id_a == id_b);
    MaterialPublished material_a, material_b;
    {
        auto result_a = wait(*a, id_a);
        auto result_b = wait(*b, id_b);
        CHECK(result_a.metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
        CHECK(result_b.metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
        material_a = result_a.get<SetMaterial::Response>(); material_b = result_b.get<SetMaterial::Response>();
        CHECK(material_a.material != material_b.material);
    }
    a.reset();
    input.material = material_a.material;
    input.expected_revision = material_a.revision;
    input.parameters[0] = {.75f, .75f, .75f, 1};
    std::optional replaced{call<SetMaterial>(*b, set_b, input)};
    CHECK(replaced->metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS && replaced->get<MaterialPublished>().revision == 2);
    {
        // A Query explicitly returns the real immutable program Owner.
        auto pending = material_a.material;
        auto result = call<QueryMaterial>(*b, query, pending);
        CHECK(result.metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
        CHECK(result.get<QueryMaterial::Response>().parameters[0][0] == .75f);
        CHECK(result.get<QueryMaterial::Response>().program.borrow<MaterialProgram>()->fragment.words.data() == original_words);
    }
    CHECK(call<ReleaseMaterial>(*b, release, material_a).metadata<ReplyHeader>().outcome == ECommandOutcome::FAILED);
    CHECK(call<ReleaseMaterial>(*b, release, replaced->get<MaterialPublished>()).metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
    CHECK(call<ReleaseMaterial>(*b, release, material_b).metadata<ReplyHeader>().outcome == ECommandOutcome::SUCCESS);
    server->stop(); thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    server.reset(); b.reset(); cooked = {};
    input = {}; other = {};
    replaced.reset();
    std::puts("Material typed operations: exact source, business failure, shared program ownership, two clients PASS");
}

