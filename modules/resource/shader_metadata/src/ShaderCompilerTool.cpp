#include <lux/engine/shader/ShaderCompiler.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>

namespace
{
    void emit(std::ostream& stream, std::string_view name, const lux::rdesc::ShaderProgramArtifact& artifact)
    {
        stream << "inline constexpr std::uint32_t " << name << "Words[] = {\n";
        for (auto word : artifact.stages.front().words)
        {
            stream << word << "u,\n";
        }
        stream << "};\ninline constexpr std::byte " << name << "Metadata[] = {\n";
        for (auto byte : lux::rdesc::ShaderInfo::serialize(artifact.interface))
        {
            stream << "std::byte{" << std::to_integer<unsigned>(byte) << "},\n";
        }
        stream << "};\n";
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc < 6)
    {
        std::fputs(
            "usage: lux_shader_compile source entry vertex|fragment|compute spirv|dxil|bundle output [-Ipath] "
            "[-Dname=value] [--symbol=name] [--physical-pointers]\n",
            stderr
        );
        return 2;
    }
    const std::string_view stage_name(argv[3]), target_name(argv[4]);
    using lux::rdesc::EShaderTarget;
    using lux::rdesc::EShaderType;
    const auto stage =
        stage_name == "vertex"
            ? EShaderType::VERTEX
            : (stage_name == "fragment" ? EShaderType::FRAGMENT
                                        : (stage_name == "compute" ? EShaderType::COMPUTE : EShaderType::UNDEFINED));
    const bool bundle = target_name == "bundle";
    if (!bundle && target_name != "spirv" && target_name != "dxil")
    {
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input)
    {
        return 3;
    }
    std::string source, symbol;
    std::vector<std::string_view> includes;
    bool physical_pointers = false;
    for (int index = 6; index < argc; ++index)
    {
        std::string_view option(argv[index]);
        if (option.starts_with("-I"))
        {
            includes.push_back(option.substr(2));
        }
        else if (option.starts_with("-D"))
        {
            std::string definition(option.substr(2));
            const auto equal = definition.find('=');
            if (equal != std::string::npos)
            {
                definition[equal] = ' ';
            }
            else
            {
                definition += " 1";
            }
            source += "#define " + definition + "\n";
        }
        else if (option.starts_with("--symbol="))
        {
            symbol = option.substr(9);
        }
        else if (option == "--physical-pointers")
        {
            physical_pointers = true;
        }
        else
        {
            return 2;
        }
    }
    if (bundle &&
        (symbol.empty() || symbol.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
                           ) != std::string::npos))
    {
        return 2;
    }
    source.append(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    const auto compile = [&](EShaderTarget target)
    { return lux::shader::compile({source, argv[2], stage, target, "sm_6_6", argv[1], includes}); };
    auto first = compile(target_name == "dxil" ? EShaderTarget::DXIL : EShaderTarget::SPIRV);
    if (!first)
    {
        std::fprintf(stderr, "%s\n", first.error().diagnostic.c_str());
        return 4;
    }
    if (bundle)
    {
        std::ofstream output(argv[5], std::ios::binary);
        output << "#pragma once\n#include <lux/engine/description/EmbeddedShader.hpp>\n";
        emit(output, symbol + "Spirv", *first);
        if (!physical_pointers)
        {
            auto second = compile(EShaderTarget::DXIL);
            if (!second)
            {
                std::fprintf(stderr, "%s\n", second.error().diagnostic.c_str());
                return 4;
            }
            emit(output, symbol + "Dxil", *second);
        }
        output << "inline constexpr lux::rdesc::EmbeddedShader " << symbol << "{lux::rdesc::EShaderType::"
               << (stage == EShaderType::VERTEX     ? "VERTEX"
                   : stage == EShaderType::FRAGMENT ? "FRAGMENT"
                                                    : "COMPUTE")
               << ", {" << symbol << "SpirvWords," << symbol << "SpirvMetadata},";
        if (physical_pointers)
        {
            output << "{}";
        }
        else
        {
            output << "{" << symbol << "DxilWords," << symbol << "DxilMetadata}";
        }
        output << "};\n";
        return output ? 0 : 3;
    }
    const auto& words = first->stages.front().words;
    const auto metadata = lux::rdesc::ShaderInfo::serialize(first->interface);
    std::ofstream output(argv[5], std::ios::binary);
    std::ofstream reflection(std::string(argv[5]) + ".meta", std::ios::binary);
    output.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(std::uint32_t));
    reflection.write(reinterpret_cast<const char*>(metadata.data()), metadata.size());
    return output && reflection ? 0 : 3;
}
