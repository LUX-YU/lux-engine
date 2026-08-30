#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/shader/ShaderAsset.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>

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
    const std::array<std::uint32_t, 5U> spirv{
        0x07230203U, 0x00010000U, 0U, 1U, 0U
    };
    auto asset = lux::asset::ShaderAsset::create(
        std::move(info),
        std::make_shared<const lux::asset::ShaderAssetData>(lux::asset::ShaderAssetData{
            lux::rdesc::Shader{spirv.data(), sizeof(spirv)},
            std::move(shader_info)
        })
    );
    assert(asset);

    constexpr lux::asset::AssetEncodeLimits encode_limits{1024U * 1024U};
    constexpr lux::asset::AssetDecodeLimits decode_limits{1024U * 1024U, 1024U * 1024U, 4U};
    const auto encoded = lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::encode(**asset, encode_limits);
    assert(encoded);
    assert(encoded->size() == 439U);
    const auto encoded_hash = sha256(*encoded);
    if (encoded_hash != "83ba75597ea45c156e89325a81bddbd5116fde1156618e47e9083a8c50d67038")
        std::cerr << "shader golden mismatch: " << encoded_hash << '\n';
    assert(encoded_hash == "83ba75597ea45c156e89325a81bddbd5116fde1156618e47e9083a8c50d67038");

    const auto decoded = lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::decode(
        id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(decoded);
    assert((*decoded)->data().shader.size() == sizeof(spirv));
    assert((*decoded)->data().info.entry_points.size() == 1U);
    const auto reencoded = lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::encode(**decoded, encode_limits);
    assert(reencoded && *reencoded == *encoded);

    auto owned = lux::cxx::SharedBytes<>::copyOf(*encoded);
    const auto inspected = lux::asset::inspectCookedAssetImage(owned, decode_limits);
    assert(inspected);
    const auto data_offset = static_cast<std::size_t>(
        inspected->data().data() - inspected->image().data()
    );
    const auto information_offset = static_cast<std::size_t>(
        inspected->information().data() - inspected->image().data()
    );

    auto malformed_spirv = *encoded;
    const std::uint32_t invalid_schema = 1U;
    std::memcpy(
        malformed_spirv.data() + data_offset + 4U * sizeof(std::uint32_t),
        &invalid_schema,
        sizeof(invalid_schema)
    );
    assert(!lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::decode(
        id(),
        lux::cxx::SharedBytes<>::copyOf(malformed_spirv),
        decode_limits
    ));

    auto malformed_reflection = *encoded;
    const std::uint32_t impossible_record_size = 0xFFFFFFFFU;
    std::memcpy(
        malformed_reflection.data() + information_offset + 1U,
        &impossible_record_size,
        sizeof(impossible_record_size)
    );
    assert(!lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::decode(
        id(),
        lux::cxx::SharedBytes<>::copyOf(malformed_reflection),
        decode_limits
    ));
    return 0;
}
