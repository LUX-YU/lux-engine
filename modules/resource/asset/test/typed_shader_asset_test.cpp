#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/shader/ShaderAsset.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    [[nodiscard]] lux::asset::AssetId id()
    {
        std::array<std::uint8_t, 16U> bytes{
            0x31U, 0x90U, 0x14U, 0x00U,
            0x00U, 0x00U, 0x40U, 0x00U,
            0x80U, 0x00U, 0x00U, 0x00U,
            0x00U, 0x00U, 0x00U, 0x09U,
        };
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] std::string sha256(std::span<const std::byte> bytes)
    {
        const auto digest = lux::cxx::algorithm::Sha256::hash(bytes);
        std::array<char, lux::cxx::algorithm::Sha256Digest::hex_size> result{};
        digest.formatHex(result);
        return {result.data(), result.size()};
    }
} // namespace

int main()
{
    lux::asset::AssetInfo info{};
    info.id = id();
    info.type = lux::asset::ShaderAsset::asset_type;
    info.date = 0x0102030405060708ULL;
    constexpr std::string_view display{"wire-contract"};
    std::memcpy(info.display_name.data(), display.data(), display.size());

    lux::rdesc::ShaderInfo shader_info{};
    shader_info.entry_points.push_back({"main", lux::rdesc::EShaderType::FRAGMENT});
    const std::array spirv{
        std::byte{0x03U}, std::byte{0x02U}, std::byte{0x23U}, std::byte{0x07U}
    };
    auto asset = lux::asset::ShaderAsset::create(
        std::move(info),
        std::make_shared<const lux::asset::ShaderAssetData>(lux::asset::ShaderAssetData{
            lux::rdesc::Shader{spirv.data(), spirv.size()},
            std::move(shader_info)
        })
    );
    assert(asset);

    constexpr lux::asset::AssetEncodeLimits encode_limits{1024U * 1024U};
    constexpr lux::asset::AssetDecodeLimits decode_limits{1024U * 1024U, 1024U * 1024U, 4U};
    const auto encoded = lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::encode(**asset, encode_limits);
    assert(encoded);
    assert(encoded->size() == 423U);
    const auto encoded_hash = sha256(*encoded);
    if (encoded_hash != "ba6d84448d95af9a83ba28d16cf8899f5c788595c2c94078b86ae6ea6ed57a9d")
        std::cerr << "shader golden mismatch: " << encoded_hash << '\n';
    assert(encoded_hash == "ba6d84448d95af9a83ba28d16cf8899f5c788595c2c94078b86ae6ea6ed57a9d");

    const auto decoded = lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::decode(
        id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(decoded);
    assert((*decoded)->data().shader.size() == spirv.size());
    assert((*decoded)->data().info.entry_points.size() == 1U);
    const auto reencoded = lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::encode(**decoded, encode_limits);
    assert(reencoded && *reencoded == *encoded);
    return 0;
}
