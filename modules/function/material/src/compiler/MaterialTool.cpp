#include <lux/engine/material/MaterialLowering.hpp>
#include <lux/engine/material/compiler/MaterialCompiler.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

int main(int argc, char** argv)
{
    const bool is_lower = argc == 4 && std::string_view(argv[1]) == "lower";
    const bool is_derive = argc == 6 && std::string_view(argv[1]) == "derive";
    if (!is_lower && !is_derive)
    {
        std::fputs(
            "lux_material_tool lower source.luxmaterial output.ir\n"
            "lux_material_tool derive input.ir output.material spirv|dxil forward,gbuffer,shadow\n",
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
    lux::asset::AssetInfo info;
    lux::asset::VMaterialAssetData payload;
    std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary;
    if (is_lower)
    {
        auto source = lux::material::decodeMaterialSource(bytes);
        if (!source)
        {
            std::fprintf(
                stderr,
                "source error %u at %s\n",
                unsigned(source.error().code),
                source.error().field.c_str()
            );
            return 4;
        }
        auto ir = lux::material::lowerMaterial(source->graph);
        if (!ir)
        {
            std::fprintf(stderr, "%s\n", ir.error().message.c_str());
            return 4;
        }
        info.id = source->id;
        info.type = lux::asset::MaterialAsset::asset_type;
        std::copy_n(
            source->name.begin(),
            std::min(source->name.size(), info.display_name.size() - 1),
            info.display_name.begin()
        );
        auto& target = payload.emplace<lux::asset::MaterialIRDescription>();
        target.ir = std::move(*ir);
        for (const auto& texture : source->graph.texture_slots)
        {
            target.texture_slot_ids.push_back(texture.texture);
        }
    }
    else
    {
        const std::string_view target_name(argv[4]);
        if (target_name != "spirv" && target_name != "dxil")
        {
            return 2;
        }
        auto image = lux::cxx::SharedBytes<>::copyOf(std::as_bytes(std::span(bytes)));
        const lux::asset::AssetDecodeLimits limits{MaxBytes, MaxBytes, 64};
        auto inspected = lux::asset::inspectCookedAssetImage(image, limits);
        if (!inspected)
        {
            return 4;
        }
        auto decoded =
            lux::asset::TAssetSerDeser<lux::asset::MaterialAsset>::decode(inspected->metadata().id, image, limits);
        if (!decoded)
        {
            std::fprintf(stderr, "material asset error %u\n", unsigned(decoded.error().code));
            return 4;
        }
        const auto* source = std::get_if<lux::asset::MaterialIRDescription>(&(*decoded)->data());
        if (!source)
        {
            std::fputs("target derivation requires an IR asset; cooked code is not portable source\n", stderr);
            return 4;
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
        auto derived = lux::material::deriveMaterial(
            source->ir,
            source->texture_slot_ids,
            {target_name == "spirv" ? lux::rdesc::EShaderTarget::SPIRV : lux::rdesc::EShaderTarget::DXIL, purposes}
        );
        if (!derived)
        {
            std::fprintf(stderr, "%s\n", derived.error().diagnostic.c_str());
            return 4;
        }
        info = (*decoded)->info();
        auxiliary.assign((*decoded)->auxiliaryPayloads().begin(), (*decoded)->auxiliaryPayloads().end());
        std::printf("program key %016llx\n", static_cast<unsigned long long>(derived->program_cache_key));
        payload = std::move(derived->material);
    }
    auto encoded = lux::asset::TAssetSerDeser<lux::asset::MaterialAsset>::encode(
        info,
        payload,
        auxiliary,
        lux::asset::AssetEncodeLimits{MaxBytes}
    );
    if (!encoded)
    {
        std::fprintf(stderr, "material encoding error %u\n", unsigned(encoded.error().code));
        return 4;
    }
    std::ofstream output(argv[3], std::ios::binary);
    output.write(reinterpret_cast<const char*>(encoded->data()), encoded->size());
    return output ? 0 : 3;
}
