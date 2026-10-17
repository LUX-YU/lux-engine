#include "Support.hpp"
#include "Allocation.hpp"
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <cstring>
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

static void setValues(PassProgram& program, float color, float depth)
{
    const std::array<float, 8> values{color, color, color, color, depth, 0, 0, 0};
    const auto bytes = std::as_bytes(std::span(values));
    program.parameter_values.assign(bytes.begin(), bytes.end());
}

int main(int argc, char** argv)
{
    CHECK(argc == 3);
    Validation validation;
    auto bootstrap = instance(validation);
    DeviceOptions options;
    options.dynamic_rendering = true;
    auto session = take(DeviceSession::create(bootstrap, options));
    const auto vertex = readSpirv(argv[1]), fragment = readSpirv(argv[2]);
    using F = lux::rdesc::ETextureFormat;
    for (auto format : {F::RGBA8_UNORM, F::RGBA16_SFLOAT, F::RGBA32_SFLOAT})
    for (auto depth_format : {F::D32_SFLOAT, F::D16_UNORM})
    for (unsigned mode = 0; mode < 6; ++mode)
    {
        const auto color_bytes = format == F::RGBA8_UNORM ? 4u : format == F::RGBA16_SFLOAT ? 8u : 16u;
        const auto depth_bytes = depth_format == F::D32_SFLOAT ? 4u : 2u;
        const TargetDesc target{16, 16, format, 1, ETargetKind::OFFSCREEN};
        RenderGraphBuilder builder;
        const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16, 1, 1, format}}));
        const auto prior = mode == 4 ? take(builder.declareResource({
            "prior", GraphImageDesc{16, 16, 1, 1, format}
        })) : color;
        const auto depth = take(builder.declareResource({"depth", GraphImageDesc{16, 16, 1, 1, depth_format}}));
        const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{256 * color_bytes}}));
        const auto depths = take(builder.declareResource({"depths", GraphBufferDesc{256 * depth_bytes}}));
        GraphPass near{"near", mode == 5 ? EGraphPassKind::TRANSFER : EGraphPassKind::GRAPHICS,
            {{{depth, 1}, {}, EGraphAccess::WRITE, 0, {},
                mode == 5 ? EGraphUse::TRANSFER_DESTINATION : EGraphUse::DEPTH_ATTACHMENT}}};
        if (mode != 3 && mode != 5) near.uses.push_back({
            {prior, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT
        });
        CHECK(builder.addPass(near));
        CHECK(builder.addPass({"far", EGraphPassKind::GRAPHICS,
            {{{depth, 2}, {}, EGraphAccess::ATTACHMENT_LOAD, 1, {}, EGraphUse::DEPTH_ATTACHMENT},
             {{color, mode >= 3 ? 1u : 2u}, {}, mode >= 3 ? EGraphAccess::WRITE : EGraphAccess::ATTACHMENT_LOAD,
                mode >= 3 ? 0u : 1u, {}, EGraphUse::COLOR_ATTACHMENT}}}));
        CHECK(builder.addPass({"read", EGraphPassKind::TRANSFER,
            {{{color, mode >= 3 ? 1u : 2u}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
        CHECK(builder.addPass({"read_depth", EGraphPassKind::TRANSFER,
            {{{depth, 2}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{depths, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
        CHECK(builder.exportOutput({{color, mode >= 3 ? 1u : 2u}, {}, target}));
        CHECK(builder.exportOutput({{pixels, 1}}));
        CHECK(builder.exportOutput({{depths, 1}}));
        auto graph = take(std::move(builder).finish());
        auto analysis = take(analyze(graph));
        PassProgram first{"near"};
        first.shader.stages = {{lux::rdesc::EShaderType::VERTEX, "main", vertex}};
        if (mode != 3) first.shader.stages.push_back({lux::rdesc::EShaderType::FRAGMENT, "main", fragment});
        first.shader.interface.parameters = ShaderBlock{32, 16};
        first.shader.interface.parameters.fields = {
            {"color", lux::rdesc::EDataScalar::FLOAT, 0, 4, 1},
            {"controls", lux::rdesc::EDataScalar::FLOAT, 16, 4, 1}
        };
        first.shader.interface.parameter_stages = mode == 3 ? 1u : 3u;
        first.graphics.depth_test = true;
        first.graphics.depth_write = mode != 1;
        first.graphics.depth_compare = mode == 2 ? EDepthCompare::GREATER : EDepthCompare::LESS;
        first.clear_depth = mode == 2 ? 0.0f : 1.0f;
        setValues(first, 0.25f, 0.25f);
        auto second = first;
        second.pass_key = "far";
        second.graphics.depth_write = true;
        if (mode == 3)
        {
            second.shader.stages.push_back({lux::rdesc::EShaderType::FRAGMENT, "main", fragment});
            second.shader.interface.parameter_stages = 3;
        }
        second.clear_color.fill(0.125f);
        setValues(second, 0.75f, 0.75f);
        if (mode == 5)
        {
            first = PassProgram{"near"};
            first.clear_depth = 0.25f;
        }
        const std::array programs{first, second, PassProgram{"read"}, PassProgram{"read_depth"}};
        auto code = std::make_shared<CompiledGraph>(take(compileGraph(
            *session, graph, analysis, {}, programs, target
        )));
        auto resized = target;
        resized.width = 32;
        resized.height = 24;
        auto* context = take(session->prepare(code, resized));
        for (unsigned frame = 0; frame < 3; ++frame)
        {
            render_allocation::participate = true;
            render_allocation::calls = 0;
            render_allocation::window = true;
            CHECK(take(context->execute()));
            CHECK(take(session->queue().wait(*context->lastUse(), UINT64_MAX)));
            render_allocation::window = false;
            CHECK(render_allocation::calls == 0);
            const auto output = take(context->readback());
            CHECK(output.size() == 32 * 24 * color_bytes);
            const float expected = mode == 0 ? 0.25f : mode >= 3 ? 0.125f : 0.75f;
            for (std::size_t i = 0; i < output.size(); i += color_bytes / 4)
            {
                if (format == F::RGBA8_UNORM)
                    CHECK(output[i] == std::byte(mode == 0 ? 64 : mode >= 3 ? 32 : 191));
                else if (format == F::RGBA16_SFLOAT)
                {
                    std::uint16_t value;
                    std::memcpy(&value, output.data() + i, sizeof(value));
                    CHECK(value == (mode == 0 ? 0x3400 : mode >= 3 ? 0x3000 : 0x3a00));
                }
                else
                {
                    float value;
                    std::memcpy(&value, output.data() + i, sizeof(value));
                    CHECK(value == expected);
                }
            }
            const auto z = take(context->readback(depths.index));
            CHECK(z.size() == 32 * 24 * depth_bytes);
            for (std::size_t i = 0; i < z.size(); i += depth_bytes)
            {
                if (depth_format == F::D32_SFLOAT)
                {
                    float value;
                    std::memcpy(&value, z.data() + i, sizeof(value));
                    CHECK(value == ((mode == 0 || mode >= 3) ? 0.25f : 0.75f));
                }
                else
                {
                    std::uint16_t value;
                    std::memcpy(&value, z.data() + i, sizeof(value));
                    CHECK(value == ((mode == 0 || mode >= 3) ? 16384 : 49151));
                }
            }
        }
        CHECK(session->release(context));
        for (unsigned fault = 0; fault < 5; ++fault)
        {
            auto bad = programs;
            if (fault == 0) bad[0].clear_depth = -1;
            if (fault == 1) bad[1].graphics.depth_compare = EDepthCompare(99);
            if (fault == 2) bad[1].shader.stages.clear();
            if (fault == 3) bad[1].shader.stages = {bad[1].shader.stages[0], bad[1].shader.stages[0]};
            if (fault == 4) bad[1].shader.stages[0].stage = lux::rdesc::EShaderType::COMPUTE;
            CHECK(!compileGraph(*session, graph, analysis, {}, bad, target));
        }
    }
    CHECK(session->drain());
    session.reset();
    CHECK(validation.errors == 0);
    std::printf("Depth compare/write/depth-only; D16/D32; RGBA8/16F/32F; resized explicit readback; hot new=0\n");
}
