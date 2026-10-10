#include "Tonemap.pass.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/graph/Builder.hpp>

using namespace lux::render;
using Format = lux::rdesc::ETextureFormat;

static void check(bool result, int line)
{
    if (!result)
    {
        std::fprintf(stderr, "F3 clear contract failure: %d\n", line);
        std::abort();
    }
}

#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), __LINE__)

static RenderResult<RenderGraphDefinition> definition(
    Format format,
    ColorClearValue clear,
    ELoadOp load = ELoadOp::CLEAR,
    ImageRange range = {}
) noexcept
{
    RenderGraphBuilder builder;
    Tonemap params;
    params.input.texture = builder.importTexture("input", {}, EPersistentScope::VIEW);
    params.linear.sampler = GraphSampler{1};
    TextureDesc texture;
    texture.format = format;
    params.output.texture = builder.importTexture("output", texture, EPersistentScope::VIEW);
    params.output.clear = clear;
    params.output.load = load;
    params.output.range = range;
    auto pass = builder.graphics("clear", GraphicsShaderReference{{"fixture", "clear"}}, EExecutionScope::VIEW, params);
    if (!pass)
    {
        return lux::cxx::unexpected(pass.error());
    }
    CHECK(builder.exportTexture(params.output.texture, PassProducer{passKey("clear")}));
    return std::move(builder).finish();
}

int main()
{
    static_assert(GraphPassParameters<Tonemap>);
    const ColorClearValue unsigned_value{UintColorClear{{16777217u, 0xffffffffu, 7u, 3u}}};
    const ColorClearValue signed_value{SintColorClear{{-1, std::numeric_limits<int>::min(), 0, 2147483647}}};
    const ColorClearValue float_value{FloatColorClear{{2.5f, -1.5f, 0.5f, 1.0f}}};
    for (auto format : {Format::R32_UINT, Format::RGBA32_UINT, Format::RGBA8_UINT})
    {
        CHECK(definition(format, unsigned_value));
        CHECK(!definition(format, signed_value));
        CHECK(!definition(format, float_value));
    }
    for (auto format : {Format::R32_SINT, Format::RGBA32_SINT, Format::RGBA8_SINT})
    {
        CHECK(definition(format, signed_value));
        CHECK(!definition(format, unsigned_value));
        CHECK(!definition(format, float_value));
    }
    for (auto format : {Format::RGBA16_SFLOAT, Format::RGBA8_SRGB, Format::RGBA8_UNORM, Format::R8_SNORM})
    {
        CHECK(definition(format, float_value));
        CHECK(!definition(format, unsigned_value));
        CHECK(!definition(format, signed_value));
    }
    CHECK(!definition(Format::D24_UNORM_S8_UINT, unsigned_value));
    CHECK(!definition(Format::D32_SFLOAT, float_value));
    CHECK(!definition(Format::UNDEFINED, float_value));
    CHECK(!definition(static_cast<Format>(999), float_value));
    CHECK(!definition(Format::R32_UINT, unsigned_value, ELoadOp::CLEAR, {EAspect::COLOR, 1, 1, 0, 1}));
    CHECK(!definition(Format::R32_UINT, unsigned_value, ELoadOp::CLEAR, {EAspect::DEPTH, 0, 1, 0, 1}));

    auto original = definition(Format::R32_UINT, unsigned_value);
    CHECK(original);
    CHECK(original->passes()[0].bindings[2].clear == unsigned_value);
    auto plan = compileLogicalGraph(*original);
    CHECK(plan);
    auto invocation = makeGraphInvocationData(*original);
    CHECK(std::get<ColorClearValue>(invocation.passes[0].fields[2]) == unsigned_value);
    const std::array imports{
        GraphImportBinding{GraphResourceId{1}, GraphBackingId{1}, 0, 1},
        GraphImportBinding{GraphResourceId{2}, GraphBackingId{2}, 0, 1}
    };
    CHECK(FrameGraphBindings::create(*plan, {}, imports, invocation));
    const ColorClearValue next{UintColorClear{{0xffffffffu, 0, 0, 0}}};
    invocation.passes[0].fields[2] = next;
    CHECK(FrameGraphBindings::create(*plan, {}, imports, invocation));
    auto changed = definition(Format::R32_UINT, next);
    CHECK(changed && plan->matches(*changed));
    auto format_changed = definition(Format::R32_SINT, signed_value);
    CHECK(format_changed && !plan->matches(*format_changed));
    invocation.passes[0].fields[2] = float_value;
    CHECK(!FrameGraphBindings::create(*plan, {}, imports, invocation));
    for (auto load : {ELoadOp::LOAD, ELoadOp::DISCARD})
    {
        auto unused_a = definition(Format::R32_UINT, float_value, load);
        auto unused_b = definition(Format::R32_UINT, signed_value, load);
        CHECK(unused_a && unused_b);
        auto unused_plan = compileLogicalGraph(*unused_a);
        CHECK(unused_plan && unused_plan->matches(*unused_b) && !plan->matches(*unused_a));
        auto unused_values = makeGraphInvocationData(*unused_b);
        CHECK(FrameGraphBindings::create(*unused_plan, {}, imports, unused_values));
    }
    std::puts("F3 typed clear: generated capture, definition, structural identity, invocation PASS");
}
