#include <lux/engine/toolchain/shader/lglsl/LglslEmitter.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

namespace
{
    using namespace lux;

    [[nodiscard]] std::string read(const char* path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) std::abort();
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
}

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    rdesc::PassResourceField texture{};
    texture.path = texture.shader_name = "textures";
    texture.role = rdesc::EPassFieldRole::SAMPLED_READ;
    texture.stages = 2;
    texture.descriptor_array = true;
    rdesc::PassResourceField data{};
    data.path = data.shader_name = "data";
    data.role = rdesc::EPassFieldRole::READ_ONLY_STORAGE;
    data.stages = 3;
    const std::array fields{texture, data};
    constexpr std::string_view values =
        "layout(std430) readonly buffer Values { vec4 color; } data;\n"
        "layout(push_constant) uniform Parameters { vec4 control; vec4 view; vec4 gain; vec4 bias; } parameters;\n";
    const std::string fragment = "uniform sampler2D textures[];\n" + std::string(values);
    const rdesc::PassShaderContract contract{"test.parameters", fields, {}, {}, 64, 16, {values, fragment, {}}};
    const auto source = read(argv[1]);
    auto emitted = shadergen::lglsl::emitPassGlsl(contract, source);
    if (!emitted)
    {
        std::cerr << emitted.error() << '\n';
        return 3;
    }
    const bool is_fragment = emitted->meta.stage == rdesc::EShaderType::FRAGMENT;
    if (emitted->injected.size() != (is_fragment ? 2u : 1u)) return 4;
    for (const auto& binding : emitted->injected)
    {
        const auto expected = binding.name == "textures" ? 0u : 1u;
        if (binding.set != 0 || binding.binding != expected) return 5;
    }
    // These are cold author errors; never silently fall back to local per-file numbering.
    if (shadergen::lglsl::emitPassGlsl(contract,
        source + "\nuniform sampler2D undeclared;\n")) return 6;
    if (shadergen::lglsl::emitPassGlsl(contract,
        source + "\nlayout(set=5, binding=7) uniform sampler2D explicit_location;\n")) return 7;
    std::ofstream output(argv[2], std::ios::binary);
    output << emitted->glsl;
    return output ? 0 : 8;
}
