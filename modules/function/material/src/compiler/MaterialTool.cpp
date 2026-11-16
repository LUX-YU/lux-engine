#include <algorithm>
#include <cstdio>
#include <fstream>
#include <lux/engine/material/compiler/MaterialCompiler.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>

int main(int argc, char** argv)
{
    if (argc != 6 || std::string_view(argv[1]) != "derive")
    {
        std::fputs(
            "lux_material_tool derive source.luxmaterial output.material spirv|dxil forward,gbuffer,shadow\n",
            stderr
        );
        return 2;
    }
    constexpr std::size_t MaxBytes = 64U * 1024U * 1024U;
    std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (!input || size < 0 || static_cast<std::uint64_t>(size) > MaxBytes)
    {
        return 3;
    }
    input.seekg(0);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!input.read(bytes.data(), size))
    {
        return 3;
    }
    auto source = lux::material::decodeMaterialSource(bytes);
    if (!source)
    {
        std::fprintf(
            stderr,
            "author source required (legacy IR must be rederived): %u at %s\n",
            unsigned(source.error().code),
            source.error().field.c_str()
        );
        return 4;
    }
    const std::string_view target_name(argv[4]);
    if (target_name != "spirv" && target_name != "dxil")
    {
        return 2;
    }
    std::vector<lux::rdesc::EMaterialPurpose> purposes;
    std::string_view names(argv[5]);
    while (!names.empty())
    {
        const auto comma = names.find(',');
        const auto name = names.substr(0, comma);
        if (name == "forward")
        {
            purposes.push_back(lux::rdesc::EMaterialPurpose::FORWARD);
        }
        else if (name == "gbuffer")
        {
            purposes.push_back(lux::rdesc::EMaterialPurpose::GBUFFER);
        }
        else if (name == "shadow")
        {
            purposes.push_back(lux::rdesc::EMaterialPurpose::SHADOW);
        }
        else
        {
            return 2;
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        names.remove_prefix(comma + 1);
        if (names.empty())
        {
            return 2;
        }
    }
    auto compiler = lux::shader::ShaderCompiler::create();
    if (!compiler)
    {
        std::fprintf(stderr, "%s\n", compiler.error().diagnostic.c_str());
        return 4;
    }
    auto derived = lux::material::deriveMaterial(
        **compiler,
        source->graph,
        {target_name == "spirv" ? lux::rdesc::EShaderTarget::SPIRV : lux::rdesc::EShaderTarget::DXIL, purposes}
    );
    if (!derived)
    {
        std::fprintf(stderr, "%s\n", derived.error().diagnostic.c_str());
        return 4;
    }
    if (!derived->diagnostic.empty())
    {
        std::fputs(derived->diagnostic.c_str(), stderr);
    }
    lux::asset::AssetInfo info;
    info.id = source->id;
    info.type = lux::asset::MaterialAsset::asset_type;
    std::copy_n(
        source->name.begin(),
        std::min(source->name.size(), info.display_name.size() - 1),
        info.display_name.begin()
    );
    auto encoded = lux::asset::TAssetSerDeser<lux::asset::MaterialAsset>::encode(
        info,
        derived->material,
        {},
        lux::asset::AssetEncodeLimits{MaxBytes}
    );
    if (!encoded)
    {
        return 4;
    }
    std::ofstream output(argv[3], std::ios::binary);
    output.write(reinterpret_cast<const char*>(encoded->data()), encoded->size());
    std::printf("program key %016llx\n", static_cast<unsigned long long>(derived->program_cache_key));
    return output ? 0 : 3;
}
