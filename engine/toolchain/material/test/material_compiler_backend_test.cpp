#include <lux/engine/material/compiler/Backend.hpp>

#include <cassert>

int main()
{
    using namespace lux::shadergen;
    using namespace lux::shadergen::glsl;

    ShaderIR empty;
    EmitParams shadow;
    shadow.pass = EMaterialPass::SHADOW;
    assert(!emitGlsl(empty, shadow));

    ShaderIR raw;
    ShaderIRValue value{};
    value.op = EOp::RAW_EXPR;
    value.type = EValueType::FLOAT;
    value.slot = 0U;
    raw.values.push_back(value);
    raw.raw_blocks.push_back(RawBlock{"hlsl", "$0"});
    raw.outputs.push_back(Output{"base_color", 0U, EValueType::FLOAT, {0.0F, 0.0F, 0.0F, 0.0F}});
    EmitParams gbuffer;
    gbuffer.pass = EMaterialPass::GBUFFER;
    assert(!emitGlsl(raw, gbuffer));
    return 0;
}
