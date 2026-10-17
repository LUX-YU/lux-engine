#include "Support.hpp"
#include "Allocation.hpp"
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <fstream>

using namespace foundation_test;

static std::vector<std::uint32_t> readSpirv(const char* path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    CHECK(input);
    const auto bytes = input.tellg();
    CHECK(bytes > 0 && bytes % 4 == 0);
    std::vector<std::uint32_t> result(static_cast<std::size_t>(bytes) / 4);
    input.seekg(0);
    CHECK(input.read(reinterpret_cast<char*>(result.data()), bytes));
    return result;
}

int main(int argc, char** argv)
{
    CHECK(argc == 4);
    Validation validation;
    auto bootstrap = instance(validation);
    DeviceOptions options;
    options.dynamic_rendering = true;
    auto session = take(DeviceSession::create(bootstrap, options));
    const TargetDesc target{16, 16, lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    RenderGraphBuilder builder;
    const auto vertex = take(builder.declareResource({"vertices", GraphBufferDesc{48}}));
    const auto index = take(builder.declareResource({"indices", GraphBufferDesc{12}}));
    const auto indirect = take(builder.declareResource({"commands", GraphBufferDesc{20}}));
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{1024}}));
    CHECK(builder.addPass({"prepare", EGraphPassKind::COMPUTE,
        {{{vertex, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::STORAGE_WRITE},
         {{index, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::STORAGE_WRITE},
         {{indirect, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::STORAGE_WRITE}}}));
    CHECK(builder.addPass({"draw", EGraphPassKind::GRAPHICS,
        {{{vertex, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::VERTEX},
         {{index, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::INDEX},
         {{indirect, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::INDIRECT},
         {{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT}}}));
    CHECK(builder.addPass({"read", EGraphPassKind::TRANSFER,
        {{{color, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
         {{pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
    CHECK(builder.exportOutput({{color, 1}, {}, target}));
    CHECK(builder.exportOutput({{pixels, 1}}));
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyze(graph));
    ShaderBlock vertex_block{48, 16}, index_block{12, 4}, command_block{20, 4};
    for (unsigned i = 0; i < 3; ++i)
    {
        vertex_block.fields.push_back({"position[" + std::to_string(i) + "]",
            lux::rdesc::EDataScalar::FLOAT, i * 16, 4, 1, 16});
        index_block.fields.push_back({"index[" + std::to_string(i) + "]",
            lux::rdesc::EDataScalar::UINT, i * 4, 1, 1, 4});
    }
    for (auto name : {"count", "instances", "first", "base", "first_instance"})
        command_block.fields.push_back({name, std::string_view(name) == "base" ? lux::rdesc::EDataScalar::SINT :
            lux::rdesc::EDataScalar::UINT, std::uint32_t(command_block.fields.size()) * 4, 1, 1});
    PassProgram prepare{"prepare"}, draw{"draw"};
    prepare.shader.stages = {{lux::rdesc::EShaderType::COMPUTE, "main", readSpirv(argv[1])}};
    prepare.shader.interface.bindings = {
        {3, 1, EDescriptorKind::STORAGE_BUFFER, 1, vertex_block, "vertices", 4, EStorageAccess::WRITE_ONLY},
        {3, 2, EDescriptorKind::STORAGE_BUFFER, 1, index_block, "indices", 4, EStorageAccess::WRITE_ONLY},
        {3, 3, EDescriptorKind::STORAGE_BUFFER, 1, command_block, "commands", 4, EStorageAccess::WRITE_ONLY}
    };
    prepare.resources = {{"vertices", "vertices"}, {"indices", "indices"}, {"commands", "commands"}};
    draw.shader.stages = {
        {lux::rdesc::EShaderType::VERTEX, "main", readSpirv(argv[2])},
        {lux::rdesc::EShaderType::FRAGMENT, "main", readSpirv(argv[3])}
    };
    draw.graphics.vertex_bindings = {{0, 16}};
    draw.graphics.vertex_attributes = {{0, 0, 0, lux::rdesc::ETextureFormat::RGBA32_SFLOAT}};
    draw.vertex_buffers = {{"vertices", 0, 0}};
    const std::array<VDrawCommand, 3> commands{
        DrawVertices{3}, DrawIndexed{"indices", 0, EIndexType::UINT32, 3},
        DrawIndexedIndirect{"indices", 0, EIndexType::UINT32, "commands"}
    };
    for (const auto& command : commands)
    {
        draw.draw = command;
        const std::array programs{prepare, draw, PassProgram{"read"}};
        auto code = std::make_shared<CompiledGraph>(take(compileGraph(
            *session, graph, analysis, {}, programs, target)));
        auto* context = take(session->prepare(code, target));
        for (unsigned i = 0; i < 5; ++i)
        {
            render_allocation::participate = true;
            render_allocation::calls = 0;
            render_allocation::window = true;
            CHECK(take(context->execute()) == EExecutionOutcome::EXECUTED);
            CHECK(take(session->queue().wait(*context->lastUse(), UINT64_MAX)));
            render_allocation::window = false;
            CHECK(render_allocation::calls == 0);
            for (auto value : take(context->readback())) CHECK(value == std::byte{64});
        }
        CHECK(session->release(context));
    }
    draw.draw = DrawIndexed{"indices", 0, EIndexType::UINT32, 3};
    for (unsigned fault = 0; fault < 9; ++fault)
    {
        auto bad = draw;
        if (fault == 0) bad.vertex_buffers[0].resource_key = "absent";
        if (fault == 1) bad.graphics.vertex_attributes[0].format = lux::rdesc::ETextureFormat::RGBA32_UINT;
        if (fault == 2) bad.graphics.vertex_attributes[0].offset = 8;
        if (fault == 3) bad.graphics.vertex_bindings[0].stride = 0;
        if (fault == 4) std::get<DrawIndexed>(bad.draw).count = 4;
        if (fault == 5) std::get<DrawIndexed>(bad.draw).offset = 1;
        if (fault == 6) bad.draw = DrawVertices{4};
        if (fault == 7) bad.draw = DrawIndexedIndirect{"indices", 0, EIndexType::UINT32, "commands", 4};
        if (fault == 8) bad.graphics.vertex_attributes.push_back(bad.graphics.vertex_attributes[0]);
        const std::array programs{prepare, bad, PassProgram{"read"}};
        CHECK(!compileGraph(*session, graph, analysis, {}, programs, target));
    }
    CHECK(session->drain());
    session.reset();
    CHECK(validation.errors == 0);
    std::printf("GPU-produced Vertex/Index/Indirect, three real draw variants; RGBA64; hot new=0\n");
}
