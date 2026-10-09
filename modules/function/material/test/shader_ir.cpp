#include <lux/engine/material/ShaderIR.hpp>

#include <cstdio>
#include <cstdlib>

namespace
{
    void require(bool condition) noexcept
    {
        if (!condition)
        {
            std::abort();
        }
    }

    std::uint64_t fingerprint(const lux::shadergen::ShaderIR& value) noexcept
    {
        const auto result = lux::shadergen::computeFingerprint(value);
        std::printf("%016llx\n", static_cast<unsigned long long>(result));
        return result;
    }
} // namespace

int main()
{
    using namespace lux::shadergen;
    ShaderIR ir;
    fingerprint(ir);
    ir.values.push_back({EOp::CONSTANT, EValueType::VEC3});
    ir.values[0].constant[0] = 0.25F;
    ir.outputs.push_back({"base_color", 0, EValueType::VEC3});
    auto previous = fingerprint(ir);
    ir.fingerprint = 919;
    require(fingerprint(ir) == previous);
    ir.outputs[0].dflt[0] = 0.75F;
    require(fingerprint(ir) == previous); // connected output does not consume its default
    ir.outputs[0].value_id = kNoValue;
    previous = fingerprint(ir);
    ir.outputs[0].dflt[0] = 0.5F;
    require(fingerprint(ir) != previous);
    ir.params.push_back({"parameter", EValueType::FLOAT, {1, 0, 0, 0}});
    previous = fingerprint(ir);
    ir.params[0].dflt[0] = 2;
    require(fingerprint(ir) == previous); // parameter defaults live in runtime data, not shader code
    ir.params[0].name = "renamed";
    require(fingerprint(ir) != previous);
    ir.inputs.push_back({"uv0", EValueType::VEC2, 3, EInterpolation::SMOOTH});
    previous = fingerprint(ir);
    ir.inputs[0].interpolation = EInterpolation::FLAT;
    require(fingerprint(ir) != previous);
    ir.inputs[0].location = 4;
    fingerprint(ir);
    ir.textures.push_back({"albedo"});
    fingerprint(ir);
    ir.raw_blocks.push_back({"glsl", "vec3($0)"});
    previous = fingerprint(ir);
    ir.raw_blocks[0].code = "vec3(1.0 - $0)";
    require(fingerprint(ir) != previous);
    ir.values[0].op = EOp::RAW_EXPR;
    fingerprint(ir);
    std::puts("PASS: expression fingerprint contract");
}
