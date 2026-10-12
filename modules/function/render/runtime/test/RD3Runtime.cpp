#include "Allocation.hpp"
#include "F3Tonemap.pass.hpp"
#include "F4Fill.pass.hpp"
#include "F4HostRead.pass.hpp"
#include "NativeGraphSupport.hpp"
#include <cstring>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/runtime/DeviceSession.hpp>
#include <thread>

using namespace native_graph_test;

RenderResult<RuntimeReply> receive(RuntimeRequest request)
{
    for (unsigned i = 0; i < 5000; ++i)
    {
        auto result = request.poll();
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        if (*result)
        {
            return **result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(false);
    return lux::cxx::unexpected(RenderError{kBusy});
}

RuntimeReply await(RuntimeRequest request, std::source_location at = std::source_location::current())
{
    return checked(receive(std::move(request)), at);
}

void frames(RenderRuntime& client, std::uint64_t count)
{
    for (unsigned i = 0; i < 5000; ++i)
    {
        if (client.progress().render_frames >= count)
        {
            return;
        }
        if (auto error = client.terminalError())
        {
            std::fprintf(stderr, "SERVER ERROR %llu %llu\n", (unsigned long long)error->type, error->args[0]);
            CHECK(false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(false);
}

struct Fixture
{
    EffectSource source;
    GraphPassId graphics, readback;
    std::uint32_t scalar_offset;
};

ProgramSource programSource(
    GraphPassId pass,
    std::string name,
    lux::rdesc::PassShaderContract schema,
    const std::filesystem::path& directory,
    bool compute
)
{
    ShaderBuildInputs inputs{name, "default", "glslc VulkanSDK 1.4.304.0", "vulkan1.3", {}, {}};
    std::vector<ShaderStageBinary> stages;
    if (compute)
    {
        inputs.sources.push_back({name + ".comp.glsl", text(directory / (name + ".comp.glsl"))});
        stages.push_back({4, words(directory / (name + ".comp.spv"))});
    }
    else
    {
        inputs.sources.push_back({name + ".vert.glsl", text(directory / (name + ".vert.glsl"))});
        inputs.sources.push_back({name + ".frag.glsl", text(directory / (name + ".frag.glsl"))});
        stages.push_back({1, words(directory / (name + ".vert.spv"))});
        stages.push_back({2, words(directory / (name + ".frag.spv"))});
    }
    auto variant = makeCompiledShaderVariant(std::move(inputs), std::move(stages), schema);
    if (!variant)
    {
        std::fprintf(stderr, "%s\n", variant.error().c_str());
    }
    CHECK(variant);
    const std::array schemas{schema};
    std::vector<OwnerShape> owners;
    ShaderOwnerId local;
    if (std::any_of(
            schema.resources.begin(),
            schema.resources.end(),
            [](const auto& value)
            {
                return lux::rdesc::isShaderDescriptorRole(value.role) &&
                       value.owner == lux::rdesc::EFieldOwner::PASS_LOCAL;
            }
        ))
    {
        owners.push_back(checked(declareOwnerShape(name + ".local", 1, lux::rdesc::EFieldOwner::PASS_LOCAL, schemas)));
        local = owners.back().identity;
    }
    std::vector<ProgramAssignment> field_owners;
    for (const auto& field : schema.resources)
    {
        if (lux::rdesc::isShaderDescriptorRole(field.role) && field.owner == lux::rdesc::EFieldOwner::PASS_LOCAL)
        {
            field_owners.push_back({std::string{field.path}, local});
        }
    }
    GraphicsDescription graphics;
    graphics.color_formats = {VK_FORMAT_R32G32B32A32_SFLOAT};
    graphics.blends.resize(1);
    VPipelineDescription description =
        compute ? VPipelineDescription{ComputeDescription{}} : VPipelineDescription{graphics};
    return {pass, std::move(*variant), schema, {std::move(field_owners), std::move(owners)}, std::move(description)};
}

Fixture effect(const std::filesystem::path& shaders, float exposure = 1.0f)
{
    RenderGraphBuilder graph;
    TextureDesc texture;
    texture.width = texture.height = 4;
    const auto hdr = graph.texture(texture, "rd3.hdr");
    const auto ldr = graph.texture(texture, "rd3.ldr");
    F4Fill fill;
    fill.output.texture = hdr;
    const auto compute = checked(graph.compute("fill", {{"F4Fill", "default"}}, EExecutionScope::SCENE, fill));
    F3Tonemap tonemap;
    tonemap.exposure = exposure;
    tonemap.input.texture = hdr;
    tonemap.nearest.sampler = GraphSampler{1};
    tonemap.output.texture = ldr;
    const auto graphics =
        checked(graph.graphics("tonemap", {{"F3Tonemap", "default"}}, EExecutionScope::VIEW, tonemap));
    F4HostRead read;
    read.source.texture = ldr;
    const auto host = checked(graph.readback("readback", EExecutionScope::VIEW, read));
    CHECK(graph.source(passKey("readback"), "source", PassProducer{passKey("tonemap")}));
    auto definition = checked(std::move(graph).finish());
    auto logical = checked(compileLogicalGraph(definition));
    const auto offset = logical.identity().passes[graphics.value() - 1].scalar_fields[0].offset;
    std::vector<ProgramSource> programs;
    programs.push_back(programSource(compute, "F4Fill", PassSchema<F4Fill>::contract(), shaders, true));
    programs.push_back(programSource(graphics, "F3Tonemap", PassSchema<F3Tonemap>::contract(), shaders, false));
    std::vector commands{
        NativePassCommand{compute, DispatchCommand{4, 4, 1}, EQueueRole::COMPUTE},
        NativePassCommand{graphics, DrawCommand{}, EQueueRole::GRAPHICS},
        NativePassCommand{host, CopyCommand{}, EQueueRole::TRANSFER}
    };
    return {{std::move(definition), std::move(programs), std::move(commands), 4, 4}, graphics, host, offset};
}

template <typename Tag> void generations()
{
    lux::cxx::SlotMap<int, Tag, std::uint32_t, std::uint8_t> objects;
    const auto old = objects.emplace(7);
    CHECK(objects.find(old) && *objects.find(old) == 7);
    auto key = old;
    for (unsigned i = 0; i < 253; ++i)
    {
        CHECK(objects.erase(key));
        CHECK(!objects.find(key));
        key = objects.emplace(8);
    }
    CHECK(key.index == old.index && key.gen == 254);
    objects.clear();
    CHECK(!objects.find(key) && !objects.find(old));
    const auto retired = objects.emplace(9);
    CHECK(retired.index != old.index);
    CHECK(!objects.erase(old));
}

void identities()
{
    generations<RenderSceneTag>();
    generations<RenderViewTag>();
    generations<RenderTargetTag>();
    generations<RenderEffectTag>();
    generations<RenderGraphTag>();
    generations<ResourceDomainTag>();
    lux::cxx::SlotMap<int, RenderGraphTag> graphs;
    lux::cxx::SlotKeySparseSet<RenderGraphId, std::vector<ExecutionJob>> groups;
    const auto a = graphs.emplace(1);
    const auto b = graphs.emplace(2);
    groups.insert(a, std::vector<ExecutionJob>{});
    groups.insert(b, std::vector<ExecutionJob>{});
    CHECK(groups.contains(a) && groups.contains(b));
    CHECK(groups.erase(a));
    CHECK(graphs.erase(a));
    const auto replacement = graphs.emplace(3);
    CHECK(replacement.index == a.index && replacement.gen != a.gen);
    groups.insert(replacement, std::vector<ExecutionJob>{});
    CHECK(!groups.tryGet(a) && groups.tryGet(b) && groups.tryGet(replacement));
    groups.clear();
    CHECK(!groups.tryGet(replacement));
    std::puts("RD3 identity allocation/erase/clear/retirement and auxiliary graph index PASS");
}

void pixel(RenderRuntime& client, RuntimeBinding binding, GraphPassId readback, float exposure)
{
    auto reply = await(checked(client.readback(binding, readback, 0, 0, 16)));
    std::array<float, 4> pixel{};
    std::memcpy(pixel.data(), reply.bytes.data(), sizeof(pixel));
    for (unsigned c = 0; c < 3; ++c)
    {
        const float input = float(c + 1);
        CHECK(std::abs(pixel[c] - input * exposure / (1 + input * exposure)) < 1e-6f);
    }
    CHECK(pixel[3] == 1);
}

void gpu(const std::filesystem::path& shaders, unsigned fif, Validation& validation)
{
    BackendOptions native;
    native.instance = {{}, true, diagnostic, &validation};
    native.device.dynamic_rendering = true;
    native.device.multiple_queues = true;
    native.capacity.frames_in_flight = fif;
    RuntimeOptions options;
    options.frame_interval_ns = 2'000'000;
    auto server = checked(RenderBackend::create(native, options));
    auto fixture = effect(shaders);
    const auto graphics = fixture.graphics;
    const auto readback = fixture.readback;
    auto values = makeGraphInvocationData(fixture.source.definition).passes[graphics.value() - 1].scalars;
    const auto offset = fixture.scalar_offset;
    const auto selected = checked(server->addEffect(std::move(fixture.source)));
    auto alternate_fixture = effect(shaders, 7.0f);
    const auto alternate = checked(server->addEffect(std::move(alternate_fixture.source)));
    auto client = checked(server->start());
    CHECK(!server->start() && !server->assemblyDevice());
    const auto owner = client->ownerIdentity();
    std::array<RuntimeScene, 2> scenes;
    for (auto& scene : scenes)
    {
        const auto reply = await(checked(client->createScene()));
        scene = {owner, {reply.index, reply.generation}};
    }
    std::array<RuntimeTarget, 4> targets;
    std::array<RuntimeView, 4> views;
    std::array<RenderGraphId, 4> graphs;
    std::array<RuntimeBinding, 4> bindings;
    for (unsigned i = 0; i < 4; ++i)
    {
        const auto target = await(checked(client->createTarget(4, 4)));
        targets[i] = {owner, {target.index, target.generation}};
        const auto view = await(checked(client->createView(scenes[i / 2])));
        views[i] = {owner, {{view.scene_index, view.scene_generation}, {view.index, view.generation}}};
        bindings[i] = {selected, views[i], targets[i]};
        await(checked(client->attachEffect(selected, views[i], targets[i])));
        await(checked(client->attachEffect(selected, views[i], targets[i]))); // duplicate is idempotent
        const auto graph = await(checked(client->graph(bindings[i])));
        graphs[i] = {graph.index, graph.generation};
        const float exposure = float(i + 1);
        std::memcpy(values.data() + offset, &exposure, sizeof(exposure));
        CHECK(client->publishBindingPass(bindings[i], graphics, 1, values));
    }
    CHECK(views[0].local.view == views[2].local.view);
    CHECK(views[0].local.scene != views[2].local.scene);
    CHECK(std::all_of(graphs.begin(), graphs.end(), [&](auto id) { return id == graphs[0]; }));
    frames(*client, client->progress().render_frames + 12);
    CHECK(client->progress().native_compilations == 1 && client->progress().native_cache_hits == 1);
    CHECK(client->progress().reused_scene_passes >= 12);
    for (unsigned i = 0; i < 4; ++i)
    {
        pixel(*client, bindings[i], readback, float(i + 1));
        const auto output = await(checked(client->readbackTarget(bindings[i], 0, 16)));
        float red{};
        std::memcpy(&red, output.bytes.data(), sizeof(red));
        const float expected = float(i + 1) / float(i + 2);
        CHECK(std::abs(red - expected) < 1e-6f);
    }

    const auto extra_target_reply = await(checked(client->createTarget(4, 4)));
    RuntimeTarget extra_target{owner, {extra_target_reply.index, extra_target_reply.generation}};
    await(checked(client->attachEffect(alternate, views[0], extra_target)));
    RuntimeBinding extra{alternate, views[0], extra_target};
    frames(*client, client->progress().render_frames + 1);
    const auto extra_graph = await(checked(client->graph(extra)));
    CHECK((RenderGraphId{extra_graph.index, extra_graph.generation} == graphs[0]));
    CHECK(!receive(checked(client->attachEffect(alternate, views[1], targets[0]))));
    CHECK(!receive(checked(client->remove(views[0]))));
    frames(*client, client->progress().render_frames + 4);
    pixel(*client, extra, readback, 7.0f);
    pixel(*client, bindings[0], readback, 1.0f);
    await(checked(client->detachEffect(alternate, views[0], extra_target)));
    await(checked(client->remove(extra_target)));

    allocation_count = allocation_bytes = 0;
    measuring = true;
    frames(*client, client->progress().render_frames + 12);
    measuring = false;
    CHECK(allocation_count == 0 && allocation_bytes == 0);

    auto foreign = bindings[0];
    ++foreign.view.owner;
    CHECK(!client->graph(foreign));
    auto wrong_scene = bindings[0];
    ++wrong_scene.view.local.scene.gen;
    CHECK(!receive(checked(client->graph(wrong_scene))));
    CHECK(!receive(checked(client->remove(scenes[0]))));
    CHECK(!receive(checked(client->remove(selected))));
    CHECK(!receive(checked(client->remove(targets[0]))));

    CHECK(!receive(checked(client->remove(views[0]))));
    await(checked(client->detachEffect(bindings[0].effect, views[0], targets[0])));
    CHECK(!receive(checked(client->detachEffect(bindings[0].effect, views[0], targets[0]))));
    await(checked(client->remove(views[0])));
    CHECK(!receive(checked(client->graph(bindings[0]))));
    const auto replacement = await(checked(client->createView(scenes[0])));
    RuntimeView recreated{
        owner,
        {{replacement.scene_index, replacement.scene_generation}, {replacement.index, replacement.generation}}
    };
    CHECK(recreated.local.view.index == views[0].local.view.index);
    CHECK(recreated.local.view.gen != views[0].local.view.gen);
    CHECK(!receive(checked(client->graph(bindings[0]))));
    pixel(*client, bindings[2], readback, 3.0f);
    await(checked(client->attachEffect(alternate, recreated, targets[0])));
    RuntimeBinding recreated_binding{alternate, recreated, targets[0]};
    frames(*client, client->progress().render_frames + 3);
    pixel(*client, recreated_binding, readback, 7.0f);
    const auto shared_again = await(checked(client->graph(recreated_binding)));
    CHECK((RenderGraphId{shared_again.index, shared_again.generation} == graphs[0]));
    views[0] = recreated;
    bindings[0] = recreated_binding;

    CHECK(!receive(checked(client->remove(views[0]))));
    await(checked(client->detachEffect(bindings[0].effect, views[0], targets[0])));
    CHECK(!receive(checked(client->detachEffect(bindings[0].effect, views[0], targets[0]))));
    await(checked(client->remove(views[0])));
    await(checked(client->remove(alternate)));
    pixel(*client, bindings[2], readback, 3.0f);
    await(checked(client->detachEffect(selected, views[1], targets[1])));
    await(checked(client->remove(views[1])));
    await(checked(client->remove(scenes[0])));
    const auto new_scene = await(checked(client->createScene()));
    CHECK(new_scene.index == scenes[0].local.index && new_scene.generation != scenes[0].local.gen);
    CHECK(!receive(checked(client->remove(scenes[0]))));
    await(checked(client->remove(targets[0])));
    const auto new_target = await(checked(client->createTarget(4, 4)));
    CHECK(new_target.index == targets[0].local.index && new_target.generation != targets[0].local.gen);
    CHECK(!receive(checked(client->remove(targets[0]))));

    auto other_server = checked(RenderBackend::create(native, options));
    auto other_client = checked(other_server->start());
    CHECK(other_client->ownerIdentity() != owner);
    const auto other_scene = await(checked(other_client->createScene()));
    CHECK(other_scene.index == scenes[0].local.index && other_scene.generation == scenes[0].local.gen);
    CHECK(!receive(checked(other_client->remove(scenes[0]))));
    CHECK(!receive(checked(other_client->attachEffect(selected, views[2], targets[2]))));
    RuntimeView other_view = views[2];
    other_view.owner = other_client->ownerIdentity();
    CHECK(!client->attachEffect(selected, other_view, targets[2]));
    other_server.reset();
    CHECK(other_client->state() == ERuntimeState::STOPPED);

    // Published reply storage and Client survive independently of Server and Native teardown.
    auto late = checked(client->graph(bindings[2]));
    auto abandoned = checked(client->graph(bindings[3]));
    server.reset();
    CHECK(client->state() == ERuntimeState::STOPPED);
    auto late_result = late.poll();
    CHECK((late_result && *late_result) || (!late_result && late_result.error().type == kTransportStopping));
    CHECK(!client->createScene());
    CHECK(!client->terminalError());
    CHECK(!client->pollDiagnostic());
    client.reset();
    CHECK(abandoned.abandon()); // the inbox remains valid after both Client and Server
    std::printf("RD3 GPU scenes=2 views=4 shared_graph=1 FIF=%u isolation/Scene reuse/late teardown PASS\n", fif);
}

#include "ResourceDomains.hpp"

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    identities();
    Validation validation;
    domains(validation);
    gpu(argv[1], 2, validation);
    gpu(argv[1], 3, validation);
    CHECK(validation.errors == 0);
    std::printf("validation_errors=%u\n", validation.errors.load());
    return 0;
}
