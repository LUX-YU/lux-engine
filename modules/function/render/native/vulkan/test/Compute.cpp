#include "Support.hpp"
#include "Allocation.hpp"
#include "ParameterFixture.hpp"
#include "shaders/Texture.hpp"
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <fstream>
#include <cstring>

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
    const auto data = take(builder.declareResource({"data", GraphBufferDesc{16}}));
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{1024}}));
    const auto values = take(builder.declareResource({"values", GraphBufferDesc{16}}));
    CHECK(!builder.addPass({"bad", EGraphPassKind::COMPUTE,
        {{{data, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_WRITE}}}));
    CHECK(!builder.addPass({"bad", EGraphPassKind::TRANSFER,
        {{{data, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::STORAGE_WRITE}}}));
    CHECK(!builder.addPass({"bad", EGraphPassKind::COMPUTE,
        {{{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::STORAGE_WRITE}}}));
    CHECK(builder.passCount() == 0);
    CHECK(builder.addPass({"compute", EGraphPassKind::COMPUTE,
        {{{data, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::STORAGE_WRITE}}}));
    CHECK(builder.addPass({"update", EGraphPassKind::COMPUTE,
        {{{data, 2}, {}, EGraphAccess::READ_WRITE, 1, {}, EGraphUse::STORAGE_READ_WRITE}}}));
    CHECK(builder.addPass({"draw", EGraphPassKind::GRAPHICS,
        {{{data, 2}, {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
         {{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT}}}));
    CHECK(builder.addPass({"read", EGraphPassKind::TRANSFER,
        {{{color, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
         {{pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
    CHECK(builder.addPass({"copy", EGraphPassKind::TRANSFER,
        {{{data, 2}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
         {{values, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
    CHECK(builder.exportOutput({{color, 1}, {}, target}));
    CHECK(builder.exportOutput({{pixels, 1}}));
    CHECK(builder.exportOutput({{values, 1}}));
    auto definition = take(std::move(builder).finish());
    auto analysis = take(analyze(definition));
    PassProgram compute{"compute"}, update{"update"}, draw{"draw"};
    compute.shader.stages = {{lux::rdesc::EShaderType::COMPUTE, "main", readSpirv(argv[1])}};
    compute.shader.interface.bindings = {{3, 7, EDescriptorKind::STORAGE_BUFFER, 1,
        parameter_fixture::float4("color"), "data", 4, EStorageAccess::WRITE_ONLY}};
    compute.resources = {{"data", "data"}};
    update = compute;
    update.pass_key = "update";
    update.shader.stages[0].words = readSpirv(argv[2]);
    update.shader.interface.bindings[0].access = EStorageAccess::READ_WRITE;
    draw.shader.stages = {
        {lux::rdesc::EShaderType::VERTEX, "main", {texture_fixture::vert.begin(), texture_fixture::vert.end()}},
        {lux::rdesc::EShaderType::FRAGMENT, "main", readSpirv(argv[3])}
    };
    draw.shader.interface.bindings = {{2, 3, EDescriptorKind::STORAGE_BUFFER, 1,
        parameter_fixture::float4("color"), "data", 2}};
    draw.resources = {{"data", "data"}};
    std::array programs{compute, update, draw, PassProgram{"read"}, PassProgram{"copy"}};
    auto code = std::make_shared<CompiledGraph>(take(compileGraph(
        *session, definition, analysis, {}, programs, target)));
    auto* execution = take(session->prepare(code, target));
    for (unsigned i = 0; i < 10; ++i)
    {
        render_allocation::participate = true;
        render_allocation::calls = 0;
        render_allocation::window = true;
        CHECK(take(execution->execute()));
        CHECK(take(session->queue().wait(*execution->lastUse(), UINT64_MAX)));
        render_allocation::window = false;
        CHECK(render_allocation::calls == 0);
        const auto rendered = take(execution->readback());
        const auto copied = take(execution->readback(values.index));
        std::array<float, 4> actual{};
        CHECK(copied.size() == sizeof(actual));
        std::memcpy(actual.data(), copied.data(), copied.size());
        std::fprintf(stderr, "iteration%u RGBA=%u,%u,%u,%u buffer=%g,%g,%g,%g\n", i,
            std::to_integer<unsigned>(rendered[0]), std::to_integer<unsigned>(rendered[1]),
            std::to_integer<unsigned>(rendered[2]), std::to_integer<unsigned>(rendered[3]),
            actual[0], actual[1], actual[2], actual[3]);
        for (auto byte : rendered) CHECK(byte == std::byte{64});
        for (auto value : actual) CHECK(value == 0.25f);
    }
    for (unsigned fault = 0; fault < 6; ++fault)
    {
        auto bad = programs;
        if (fault == 0) bad[0].dispatch[0] = 0;
        if (fault == 1) bad[0].dispatch[1] = UINT32_MAX;
        if (fault == 2) bad[0].shader.interface.bindings[0].access = EStorageAccess::READ_ONLY;
        if (fault == 3) bad[0].shader.stages[0].entry = "absent";
        if (fault == 4) bad[1].shader.stages[0].words = compute.shader.stages[0].words;
        if (fault == 5) bad[0].shader.interface.bindings[0].stages = 6;
        CHECK(!compileGraph(*session, definition, analysis, {}, bad, target));
    }
    CHECK(session->release(execution));
    CHECK(session->drain());
    code.reset();
    session.reset();
    CHECK(validation.errors == 0);
    std::printf("Compute write/read-write -> Graphics + Buffer copy; RGBA64/float0.25; hot new=0\n");
}
