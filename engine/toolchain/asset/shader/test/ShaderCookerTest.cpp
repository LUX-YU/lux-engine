#include <lux/engine/resource/asset/shader/ShaderAsset.hpp>
#include <lux/engine/toolchain/asset/shader/ShaderCooker.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace
{
    [[nodiscard]] lux::asset::AssetInfo metadata()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = 1U;
        lux::asset::AssetInfo result{};
        result.id = lux::asset::AssetId{bytes};
        result.type = lux::asset::ShaderAsset::asset_type;
        return result;
    }

    [[nodiscard]] std::vector<std::byte> readAll(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        assert(stream);
        const auto end = stream.tellg();
        assert(end > 0);
        std::vector<std::byte> result(static_cast<std::size_t>(end));
        stream.seekg(0, std::ios::beg);
        stream.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
        assert(stream);
        return result;
    }
} // namespace

int main()
{
    const auto spirv = readAll(LUX_SHADER_COOKER_TEST_SPIRV);
    const auto cooked = lux::toolchain::cookShader(
        metadata(),
        lux::cxx::SharedBytes<>::copyOf(spirv)
    );
    assert(cooked);
    assert((*cooked)->data().shader.size() == spirv.size());
    assert((*cooked)->data().info.entry_points.size() == 1U);
    assert((*cooked)->data().info.entry_points[0].stage == lux::rdesc::EShaderType::VERTEX);
    assert((*cooked)->data().info.vertex_inputs.size() == 1U);
    assert((*cooked)->data().info.vertex_inputs[0].location == 0U);

    constexpr lux::asset::AssetEncodeLimits encode_limits{1024U * 1024U};
    constexpr lux::asset::AssetDecodeLimits decode_limits{1024U * 1024U, 1024U * 1024U, 4U};
    const auto encoded = lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::encode(**cooked, encode_limits);
    assert(encoded);
    const auto decoded = lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::decode(
        (*cooked)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(decoded && (*decoded)->data().shader.size() == spirv.size());

    constexpr std::array invalid{
        std::byte{0x03U}, std::byte{0x02U}, std::byte{0x23U}, std::byte{0x07U}
    };
    const auto invalid_result = lux::toolchain::cookShader(
        metadata(),
        lux::cxx::SharedBytes<>::copyOf(invalid)
    );
    assert(!invalid_result && invalid_result.error().code == lux::toolchain::EShaderCookError::INVALID_SPIRV);
    return 0;
}
