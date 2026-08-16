#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/toolchain/asset/texture/TextureImporter.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
    [[nodiscard]] std::vector<std::byte> makePpm4x4()
    {
        constexpr std::array header{
            'P', '6', '\n', '4', ' ', '4', '\n', '2', '5', '5', '\n'
        };
        std::vector<std::byte> bytes;
        bytes.reserve(header.size() + 4u * 4u * 3u);
        for (const char value : header)
            bytes.push_back(static_cast<std::byte>(value));

        for (std::uint32_t y = 0; y < 4; ++y)
        {
            for (std::uint32_t x = 0; x < 4; ++x)
            {
                bytes.push_back(static_cast<std::byte>(x * 63u));
                bytes.push_back(static_cast<std::byte>(y * 63u));
                bytes.push_back(static_cast<std::byte>(255u - x * 31u));
            }
        }
        return bytes;
    }

    [[nodiscard]] bool encodes(
        lux::rdesc::ETexturePixelFormat format,
        const std::vector<std::byte>& source
    )
    {
        auto manager = std::make_shared<lux::asset::AssetManager>(
            lux::asset::runtimeAssetCodecCatalog()
        );
        lux::toolchain::TextureImporter importer{std::move(manager)};
        importer.config().output_format = format;
        importer.config().color_space =
            lux::rdesc::ETextureColorSpace::SRGB;

        return importer.fromMemory(source.data(), source.size()).has_value();
    }
}

int main()
{
    const auto source = makePpm4x4();
    const std::array formats{
        lux::rdesc::ETexturePixelFormat::BC1_SRGB,
        lux::rdesc::ETexturePixelFormat::BC3_SRGB,
        lux::rdesc::ETexturePixelFormat::BC5_UNORM,
        lux::rdesc::ETexturePixelFormat::BC7_SRGB
    };

    for (const auto format : formats)
    {
        if (!encodes(format, source))
        {
            std::cerr << "compressed texture import failed for format "
                      << static_cast<std::uint32_t>(format) << '\n';
            return 1;
        }
    }

    return 0;
}
