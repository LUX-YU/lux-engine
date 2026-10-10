#include LUX_FIXTURE_HEADER
#include <fstream>
#include <iostream>
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/toolchain/shader/PassValidation.hpp>
#include <lux/engine/toolchain/shader/lglsl/LglslEmitter.hpp>

using namespace lux::render;
static_assert(GraphPassParameters<LUX_FIXTURE_TYPE>);

struct Unmarked
{
    float value;
};

static_assert(!GraphPassParameters<Unmarked>);

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        return 2;
    }
    if (std::string_view(argv[2]) == "emitter-contract")
    {
        const auto contract = PassSchema<LUX_FIXTURE_TYPE>::contract();
        const auto good = lux::shadergen::lglsl::emitPassGlsl(
            contract,
            "//! lux-shader stage=fragment entry=main\n// binding is generated\nvoid main() {}\n"
        );
        const auto bad = lux::shadergen::lglsl::emitPassGlsl(
            contract,
            "//! lux-shader stage=fragment entry=main\nlayout(set\t=0, binding = 0) uniform texture2D escape;\n"
        );
        if (!good || bad || bad.error().find("cannot assign physical") == std::string::npos)
        {
            return 1;
        }
        return 0;
    }
    if (std::string_view(argv[2]) == "validate")
    {
        std::ifstream binary(argv[1], std::ios::binary | std::ios::ate);
        if (!binary)
        {
            return 2;
        }
        const auto size = binary.tellg();
        std::vector<std::uint32_t> words(static_cast<std::size_t>(size) / 4);
        binary.seekg(0);
        binary.read(reinterpret_cast<char*>(words.data()), size);
        const auto valid = lux::toolchain::validatePassSpirv(words, PassSchema<LUX_FIXTURE_TYPE>::contract());
        if (!valid)
        {
            std::cerr << valid.error();
            return 1;
        }
        std::cout << "schema/SPIR-V fields reconciled\n";
        return 0;
    }
    std::ifstream stream(argv[1]);
    std::string source((std::istreambuf_iterator<char>(stream)), {});
    const auto result = lux::shadergen::lglsl::emitPassGlsl(PassSchema<LUX_FIXTURE_TYPE>::contract(), source);
    if (!result)
    {
        std::cerr << result.error();
        return 1;
    }
    std::ofstream(argv[2]) << result->glsl;
    std::cout << "schema=" << PassSchema<LUX_FIXTURE_TYPE>::contract().canonical_name
              << " resources=" << result->injected.size() << '\n';
    return 0;
}
