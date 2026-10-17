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

static RenderCompletion wait(RenderClient& client, RequestId id)
{
    const auto deadline = Clock::now() + 15s;
    while (Clock::now() < deadline)
    {
        RenderCompletion result;
        if (take(client.tryReceive(result)) == ERenderReceive::RECEIVED)
        {
            CHECK(result.header.request == id);
            return result;
        }
        std::this_thread::yield();
    }
    std::abort();
}

template <RenderCommand C> static TypedRenderCompletion<C> call(
    RenderClient& client, const BoundCommand<C>& binding, typename C::Request input
)
{
    auto result = wait(client, take(client.submit(binding, input, 4096)));
    return take(std::move(result).take(binding));
}

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
    std::atomic<unsigned> code_released{0};
    auto pin = std::shared_ptr<const void>(new int, [&](const void* value) {
        delete static_cast<const int*>(value); ++code_released;
    });
    auto cooked = take(ConfigValue::make(std::move(program), pin));
    const auto* original_words = cooked.borrow<MaterialProgram>()->fragment.words.data();
    CpuFixture host;
    RenderServerConfig config{"runtime.material.operations", true};
    config.device.multiple_queues = true;
    config.uploads = TransferOptions{65536, 2, 131072};
    auto server = take(RenderServer::create(withCpu(host, config)));
    const auto type = take(server->registerStorage<lux::render::MaterialStorage>());
    CHECK(take(server->registerStorage<lux::render::MaterialStorage>()) == type);
    bool run_ok = false;
    std::thread thread([&] { run_ok = bool(server->run()); });
    auto a = take(RenderClient::create(config.channel_name));
    auto b = take(RenderClient::create(config.channel_name));
    CHECK(wait(*a, take(a->hello())).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(wait(*b, take(b->hello())).header.outcome == ECommandOutcome::SUCCESS);
    CHECK(runtime_test::refresh(*a) && runtime_test::refresh(*b));
    const auto storage = take(runtime_test::prepareStorage(*a, kMaterialStorageType.name(), "material"));
    CHECK(take(runtime_test::prepareStorage(*b, kMaterialStorageType.name(), "material")) == storage);
    const auto set_a = take(a->bind<SetMaterial>()), set_b = take(b->bind<SetMaterial>());
    const auto query = take(b->bind<QueryMaterial>());
    const auto release = take(b->bind<ReleaseMaterial>());
    MaterialWrite input{{storage, {}}, 7, cooked, cooked.borrow<MaterialProgram>()->defaults};
    {
        auto failed = call(*a, set_a, std::move(input));
        CHECK(failed.header().outcome == ECommandOutcome::FAILED);
        CHECK(failed.request().program.borrow<MaterialProgram>()->fragment.words.data() == original_words);
        input = std::move(failed.request());
    }
    CHECK(a->retainedBytes() == 0);
    auto other = input;
    CHECK(call(*b, set_b, std::move(other)).header().outcome == ECommandOutcome::FAILED);
    input.expected_revision = 0;
    other = input;
    const auto id_a = take(a->submit(set_a, input, 4096));
    const auto id_b = take(b->submit(set_b, other, 4096));
    CHECK(id_a == id_b);
    MaterialPublished material_a, material_b;
    {
        auto result_a = take(wait(*a, id_a).take(set_a));
        auto result_b = take(wait(*b, id_b).take(set_b));
        CHECK(result_a.header().outcome == ECommandOutcome::SUCCESS);
        CHECK(result_b.header().outcome == ECommandOutcome::SUCCESS);
        material_a = result_a.response(); material_b = result_b.response();
        CHECK(material_a.material != material_b.material);
        input = std::move(result_a.request());
    }
    CHECK(a->retainedBytes() == 0 && b->retainedBytes() == 0);
    a.reset();
    input.material = material_a.material;
    input.expected_revision = material_a.revision;
    input.parameters[0] = {.75f, .75f, .75f, 1};
    std::optional replaced{call(*b, set_b, std::move(input))};
    CHECK(replaced->header().outcome == ECommandOutcome::SUCCESS && replaced->response().revision == 2);
    {
        // A Query carries the complete immutable program backing and requires the corresponding Reply budget.
        auto pending = material_a.material;
        auto completion = wait(*b, take(b->submit(query, pending, cooked.retainedBytes() + 4096)));
        auto result = take(std::move(completion).take(query));
        CHECK(result.header().outcome == ECommandOutcome::SUCCESS);
        CHECK(result.response().parameters[0][0] == .75f);
        CHECK(result.response().program.borrow<MaterialProgram>()->fragment.words.data() == original_words);
    }
    CHECK(call(*b, release, material_a).header().outcome == ECommandOutcome::FAILED);
    CHECK(call(*b, release, replaced->response()).header().outcome == ECommandOutcome::SUCCESS);
    CHECK(call(*b, release, material_b).header().outcome == ECommandOutcome::SUCCESS);
    server->stop(); thread.join();
    CHECK(run_ok && server->stats().validation_errors == 0);
    server.reset(); b.reset(); cooked = {}; pin.reset();
    CHECK(code_released == 0);
    CHECK(replaced->request().program.borrow<MaterialProgram>()->fragment.words.data() == original_words);
    replaced.reset();
    CHECK(code_released == 1);
    std::puts("Material typed operations: exact source, failure recovery, budget, two clients, late Code Pin PASS");
}

