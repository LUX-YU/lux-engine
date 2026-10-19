#include <fstream>
#include <lux/engine/toolchain/shader/SpirvReflection.hpp>
#include <string_view>

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4)
    {
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    const auto bytes = input.tellg();
    if (!input || bytes <= 0 || bytes % 4 != 0)
    {
        return 2;
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / 4);
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(words.data()), bytes))
    {
        return 3;
    }
    lux::rdesc::ShaderInfo info;
    if (!lux::toolchain::reflectSpirv(words.data(), words.size() * 4, info))
    {
        return 4;
    }
    const auto metadata = lux::rdesc::ShaderInfo::serialize(info);
    std::ofstream output(argv[2], std::ios::binary);
    if (argc == 3)
    {
        output.write(reinterpret_cast<const char*>(metadata.data()), metadata.size());
    }
    else
    {
        const std::string_view symbol(argv[3]);
        if (symbol.empty() || symbol.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
                              ) != std::string_view::npos)
        {
            return 2;
        }
        output << "#pragma once\n#include <cstdint>\n#include <cstddef>\n";
        output << "inline constexpr std::uint32_t " << symbol << "[] = {\n";
        for (auto word : words)
        {
            output << word << "u,\n";
        }
        output << "};\ninline constexpr std::byte " << symbol << "Reflection[] = {\n";
        for (auto byte : metadata)
        {
            output << "std::byte{" << std::to_integer<unsigned>(byte) << "},\n";
        }
        output << "};\n";
    }
    return output ? 0 : 3;
}
