#include "Storage.pass.hpp"
#include "Tonemap.pass.hpp"
#include "Transfer.pass.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/graph/Builder.hpp>

using namespace lux::render;

static void check(bool value, int line)
{
    if (!value)
    {
        std::fprintf(stderr, "F2 authoring failed at %d\n", line);
        std::abort();
    }
}

#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), __LINE__)

int main()
{
    RenderGraphBuilder graph, foreign;
    auto input = graph.importTexture("scene.input", {}, EPersistentScope::SCENE);
    auto output = graph.texture({}, "view.output");
    Tonemap params;
    params.input.texture = input;
    params.linear.sampler = GraphSampler{1};
    params.output.texture = output;
    auto added =
        graph.graphics("tonemap", GraphicsShaderReference{{"tonemap", "default"}}, EExecutionScope::VIEW, params);
    CHECK(added);
    CHECK(!graph.provide(graphResourceKey("foreign"), foreign.texture({}), passKey("tonemap")));
    CHECK(!graph.source(
        passKey("tonemap"),
        "input",
        SemanticProducer{graphResourceKey("optional")},
        AuthoringFallback{foreign.texture({})}
    ));
    CHECK(graph.exportTexture(output, PassProducer{passKey("tonemap")}));
    auto definition = std::move(graph).finish();
    CHECK(definition);
    auto plan = compileLogicalGraph(*definition);
    CHECK(plan && plan->executionOrder().size() == 1);
    auto values = makeGraphInvocationData(*definition);
    const std::array imports{GraphImportBinding{GraphResourceId{input.value()}, GraphBackingId{10}, 0, 1}};
    auto first = FrameGraphBindings::create(*plan, {1, 1, 0}, imports, values);
    CHECK(first);
#if defined(LUX_TEMP_VECTOR)
    auto rejected =
        FrameGraphBindings::create(*plan, {}, std::vector<GraphImportBinding>(imports.begin(), imports.end()), values);
#elif defined(LUX_TEMP_INVOCATION)
    auto rejected = FrameGraphBindings::create(*plan, {}, imports, makeGraphInvocationData(*definition));
#elif defined(LUX_WRONG_SHADER)
    auto rejected =
        graph.graphics("wrong", ComputeShaderReference{{"compute", "default"}}, EExecutionScope::VIEW, params);
#elif defined(LUX_WRONG_STORAGE)
    std::array<int, 1> wrong_imports{};
    auto rejected = FrameGraphBindings::create(*plan, {}, wrong_imports, values);
#endif
    values.camera[0] = 0.75f;
    values.passes[0].scalars[0] = std::byte{1};
    values.passes[0].fields[1] = GraphSampler{2};
    values.passes[0].fields[2] = ColorClearValue{{0.2f, 0.3f, 0.4f, 1.0f}};
    auto second = FrameGraphBindings::create(*plan, {2, 1000, 1}, imports, values);
    CHECK(second && second->values()->camera[0] == 0.75f);
    CHECK(second->invocation(GraphPassId{1})->scalars[0] == std::byte{1});
    CHECK(std::get<GraphSampler>(second->invocation(GraphPassId{1})->fields[1]) == GraphSampler{2});
    CHECK(std::get<ColorClearValue>(second->invocation(GraphPassId{1})->fields[2]).value[0] == 0.2f);
    CHECK(&second->plan() == &first->plan());
    values.passes[0].fields[1] = ColorClearValue{};
    CHECK(!FrameGraphBindings::create(*plan, {}, imports, values));

    RenderGraphBuilder compute;
    Storage storage;
    storage.values.buffer = compute.importBuffer("values", {64, 4}, EPersistentScope::SCENE);
    storage.output.texture = compute.texture({});
    CHECK(compute.compute("compute", ComputeShaderReference{{"storage", "default"}}, EExecutionScope::VIEW, storage));
    CHECK(compute.exportTexture(storage.output.texture, PassProducer{passKey("compute")}));
    auto compute_definition = std::move(compute).finish();
    CHECK(compute_definition && compileLogicalGraph(*compute_definition));
    CHECK(!foreign.compute("invalid", ComputeShaderReference{{"wrong", "default"}}, EExecutionScope::VIEW, params));
    RenderGraphBuilder transfer;
    Transfer copy;
    copy.source.buffer = transfer.importBuffer("source", {64, 4}, EPersistentScope::RUNTIME);
    copy.destination.buffer = transfer.buffer({64, 4});
    CHECK(transfer.transfer("copy", EExecutionScope::VIEW, copy));
    CHECK(transfer.exportBuffer(copy.destination.buffer, PassProducer{passKey("copy")}));
    auto copied = std::move(transfer).finish();
    CHECK(copied && compileLogicalGraph(*copied));
    std::puts("typed graphics/compute, generated schema, binding values, owner scope: PASS");
}
