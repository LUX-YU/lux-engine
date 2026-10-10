#include "Attachments.pass.hpp"
#include "Hzb.pass.hpp"
#include "NonShader.pass.hpp"
#include "Optional.pass.hpp"
#include "Storage.pass.hpp"
#include "Tonemap.pass.hpp"
#include "Transfer.pass.hpp"
#include <cstring>
#include <iostream>
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/graph/Plan.hpp>

using namespace lux::render;

static int failures = 0;

static void check(bool condition, const char* name)
{
    if (!condition)
    {
        std::cerr << name << '\n';
        ++failures;
    }
}

static Tonemap tonemap(GraphTexture input, GraphTexture output)
{
    Tonemap value;
    value.input.texture = input;
    value.linear.sampler = GraphSampler{1};
    value.output.texture = output;
    value.exposure = 2.0f;
    return value;
}

int main()
{
    RenderGraphBuilder graph;
    const auto input = graph.importTexture("test.input", {}, EPersistentScope::SCENE);
    const auto output = graph.texture({});
    {
        auto parameters = tonemap(input, output);
        check(
            bool(graph.addPass(
                "test.pass.1",
                ShaderReference{"test.shader.2", "default"},
                EPassKind::GRAPHICS,
                EExecutionScope::VIEW,
                parameters
            )),
            "typed Tonemap accepted"
        );
        parameters.exposure = 99;
        parameters.input.texture = {};
    }
    auto definition = std::move(graph).finish();
    check(bool(definition), "owning Definition finishes after Params destruction");
    if (definition)
    {
        float exposure{};
        std::memcpy(&exposure, definition->passes()[0].initial_scalars.data() + offsetof(Tonemap, exposure), 4);
        check(exposure == 2.0f, "scalar snapshot owns original bytes");
        check(
            definition->passes()[0].scalar_fields[0].frequency == lux::rdesc::EUpdateFrequency::DRAW,
            "scalar layout and update metadata owned"
        );
        check(!definition->passes()[0].shader_declarations.empty(), "same-source shader contract owned");
        check(definition->passes()[0].uses.size() == 2, "Graph uses generated from fields");
        check(definition->passes()[0].bindings.size() == 3, "sampler retained without content dependency");
        check(definition->passes()[0].bindings[1].sampler == GraphSampler{1}, "sampler value owned");
        check(definition->passes()[0].bindings[2].load == ELoadOp::CLEAR, "attachment operation retained");
        check(definition->resources()[0].persistent_scope == EPersistentScope::SCENE, "origin and scope distinct");
    }
    RenderGraphBuilder hzb;
    TextureDesc pyramid;
    pyramid.mip_count = 5;
    pyramid.format = lux::rdesc::ETextureFormat::R32_SFLOAT;
    const auto texture = hzb.importTexture("test.input", pyramid, EPersistentScope::VIEW);
    Hzb params;
    params.source.texture = texture;
    params.source.range = {EAspect::COLOR, 0, 1, 0, 1};
    params.nearest.sampler = GraphSampler{1};
    params.destination.texture = texture;
    params.destination.range.base_mip = 1;
    check(
        bool(hzb.addPass(
            "test.pass.3",
            ShaderReference{"test.shader.4", "default"},
            EPassKind::COMPUTE,
            EExecutionScope::VIEW,
            params
        )),
        "HZB accepted"
    );
    auto hz = std::move(hzb).finish();
    check(bool(hz), "disjoint mip declarations are representable");
    if (hz)
    {
        check(hz->passes()[0].uses[1].image_range.base_mip == 1, "mip range retained");
        const auto schedule = CompiledGraphPlan::compile(*hz);
        check(
            !schedule && schedule.error().type == kGraphUnsupportedScheduling,
            "R3 scheduler cannot silently claim subresource support"
        );
    }

    RenderGraphBuilder invalid;
    const auto image = invalid.importTexture("test.input", {}, EPersistentScope::VIEW);
    params.source.texture = image;
    params.destination.texture = image;
    params.destination.range.base_mip = 9;
    check(
        bool(invalid.addPass(
            "test.pass.3",
            ShaderReference{"test.shader.4", "default"},
            EPassKind::COMPUTE,
            EExecutionScope::VIEW,
            params
        )),
        "capture candidate"
    );
    check(!std::move(invalid).finish(), "out of bounds mip fails finish");
    RenderGraphBuilder overlap;
    params.source.texture = overlap.importTexture("test.input", pyramid, EPersistentScope::VIEW);
    params.destination.texture = params.source.texture;
    params.destination.range.base_mip = 0;
    check(
        bool(overlap.addPass(
            "test.pass.3",
            ShaderReference{"test.shader.4", "default"},
            EPassKind::COMPUTE,
            EExecutionScope::VIEW,
            params
        )),
        "capture overlap"
    );
    check(!std::move(overlap).finish(), "implicit same mip feedback rejected");

    RenderGraphBuilder missing;
    check(
        !missing.addPass(
            "test.pass.1",
            ShaderReference{"test.shader.1", "default"},
            EPassKind::GRAPHICS,
            EExecutionScope::VIEW,
            Tonemap{}
        ),
        "required resources fail explicitly"
    );
    RenderGraphBuilder storage;
    Storage storage_params;
    storage_params.values.buffer = storage.importBuffer("test.buffer", {64, 4}, EPersistentScope::SCENE);
    storage_params.values.range = {~std::uint64_t{0} - 2, 8};
    storage_params.output.texture = storage.texture({});
    check(
        bool(storage.addPass(
            "test.pass.5",
            ShaderReference{"test.shader.6", "default"},
            EPassKind::COMPUTE,
            EExecutionScope::SCENE,
            storage_params
        )),
        "capture storage candidate"
    );
    check(!std::move(storage).finish(), "overflow buffer range rejected");
    static_assert(GraphPassParameters<NonShader>);
    static_assert(GraphPassParameters<Optional>);
    RenderGraphBuilder optional_graph;
    Optional optional;
    optional.group.input.fallback = optional_graph.importTexture("test.input", {}, EPersistentScope::SCENE);
    optional.linear.sampler = GraphSampler{7};
    optional.output.texture = optional_graph.texture({});
    check(
        bool(optional_graph.addPass(
            "test.pass.7",
            ShaderReference{"test.shader.8", "default"},
            EPassKind::GRAPHICS,
            EExecutionScope::VIEW,
            optional
        )),
        "nested optional input selects explicit fallback"
    );
    auto optional_definition = std::move(optional_graph).finish();
    check(bool(optional_definition), "optional owning Definition");
    if (optional_definition)
    {
        check(optional_definition->passes()[0].bindings[0].path == "group.input", "nested field identity retained");
        check(optional_definition->passes()[0].uses[0].resource.value() == 1, "fallback Graph dependency retained");
    }
    optional.group.input.fallback = {};
    RenderGraphBuilder missing_fallback;
    check(
        !missing_fallback.addPass(
            "test.pass.7",
            ShaderReference{"test.shader.8", "default"},
            EPassKind::GRAPHICS,
            EExecutionScope::VIEW,
            optional
        ),
        "optional missing fallback fails instead of null binding"
    );
    NonShader non_shader;
    non_shader.color.texture = GraphTexture{1};
    non_shader.resolved.texture = GraphTexture{2};
    non_shader.depth.texture = GraphTexture{3};
    non_shader.depth.stencil_load = ELoadOp::LOAD;
    non_shader.depth.clear_stencil = 42;
    non_shader.source.buffer = GraphBuffer{4};
    non_shader.destination.buffer = GraphBuffer{5};
    non_shader.vertices.buffer = GraphBuffer{6};
    non_shader.indices.buffer = GraphBuffer{7};
    non_shader.indirect.buffer = GraphBuffer{8};
    auto captured = PassSchema<NonShader>::capture(non_shader);
    check(!captured.error && captured.uses.size() == 8, "non shader roles captured");
    check(captured.uses[2].access == EGraphAccess::READ_WRITE, "stencil LOAD implies read");
    check(captured.bindings[2].clear_stencil == 42, "integer stencil clear retained");
    check(captured.bindings[1].paired_texture == "color", "resolve association retained");
    for (const bool multisample : {false, true})
    {
        RenderGraphBuilder attachments;
        Attachments values;
        TextureDesc color;
        color.samples = multisample ? 4 : 1;
        values.color.texture = attachments.texture(color);
        values.resolved.texture = attachments.texture({});
        color.format = lux::rdesc::ETextureFormat::D24_UNORM_S8_UINT;
        values.depth.texture = attachments.texture(color);
        values.depth.range.aspect = EAspect::DEPTH_STENCIL;
        check(
            bool(attachments.addPass(
                "test.pass.9",
                ShaderReference{"test.shader.10", "default"},
                EPassKind::GRAPHICS,
                EExecutionScope::VIEW,
                values
            )),
            "attachment candidate captured"
        );
        check(bool(std::move(attachments).finish()) == multisample, "resolve requires multisample source");
    }
    RenderGraphBuilder transfer_graph;
    Transfer transfer;
    transfer.source.buffer = transfer_graph.importBuffer("test.buffer", {16, 4}, EPersistentScope::RUNTIME);
    transfer.source.range = {0, 16};
    transfer.destination.buffer = transfer_graph.buffer({16, 4});
    transfer.destination.range = {0, 16};
    check(
        bool(transfer_graph.addPass("test.pass.11", {}, EPassKind::TRANSFER, EExecutionScope::SCENE, transfer)),
        "non Shader transfer pass needs no Shader identity"
    );
    check(bool(std::move(transfer_graph).finish()), "transfer Definition owns ranges");
    RenderGraphBuilder updated;
    const auto updated_input = updated.importTexture("test.input", {}, EPersistentScope::SCENE);
    const auto updated_output = updated.texture({});
    auto dynamic = tonemap(updated_input, updated_output);
    dynamic.exposure = 9.0f;
    dynamic.linear.sampler = GraphSampler{9};
    dynamic.output.clear[0] = 0.5f;
    check(
        bool(updated.addPass(
            "test.pass.1",
            ShaderReference{"test.shader.2", "default"},
            EPassKind::GRAPHICS,
            EExecutionScope::VIEW,
            dynamic
        )),
        "new initial dynamic values accepted"
    );
    auto same_topology = std::move(updated).finish();
    check(definition && same_topology && *definition == *same_topology, "dynamic values do not alter logical topology");
    std::cout << "builder_failures=" << failures << '\n';
    return failures != 0;
}
