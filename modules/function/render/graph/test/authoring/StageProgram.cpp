#include "Stages.pass.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <lux/engine/toolchain/shader/PassValidation.hpp>

static std::vector<std::uint32_t> read(const char* path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0 || file.tellg() % 4 != 0)
    {
        return {};
    }
    const auto size = file.tellg();
    std::vector<std::uint32_t> result(static_cast<std::size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(result.data()), size);
    return result;
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        return 2;
    }
    const auto vertex = read(argv[1]);
    const auto fragment = read(argv[2]);
    using namespace lux::toolchain;
    const auto contract = lux::render::PassSchema<Stages>::contract();
    const std::array modules{PassShaderModule{vertex}, PassShaderModule{fragment}};
    const auto good = validatePassShaders(modules, contract, 3);
    if (!good)
    {
        std::cerr << good.error();
        return 1;
    }
    const auto missing = validatePassShaders(std::span(modules).first(1), contract, 3);
    const std::array duplicate{modules[0], modules[0]};
    const auto repeated = validatePassShaders(duplicate, contract, 3);
    const auto wrong = validatePassShaders(modules, contract, 4);
    const auto vertex_only = validatePassSpirv(vertex, contract);
    const auto fragment_only = validatePassSpirv(fragment, contract);
    auto fields = lux::render::PassSchema<Stages>::resources;
    fields[1].element_stride = 16;
    auto incompatible = contract;
    incompatible.resources = fields;
    const auto mismatch = validatePassShaders(modules, incompatible, 3);
    if (!vertex_only || !fragment_only || missing || repeated || wrong || mismatch ||
        missing.error().find("Incomplete") == std::string::npos ||
        repeated.error().find("Duplicate") == std::string::npos ||
        mismatch.error().find("Uniform block size") == std::string::npos)
    {
        return 1;
    }
    std::cout << "vertex+fragment union reconciled; missing/duplicate/wrong stage/shared ABI rejected\n";
    return 0;
}
