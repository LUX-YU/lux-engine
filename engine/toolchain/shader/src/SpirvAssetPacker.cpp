#include <lux/engine/toolchain/shader/SpirvAssetPacker.hpp>

#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/toolchain/shader/SpirvReflection.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

namespace lux::toolchain
{
    namespace
    {
        constexpr std::uint32_t kSpirvMagic = 0x07230203u;

        [[nodiscard]] bool hasSpirvHeader(
            const std::vector<std::byte>& bytes
        ) noexcept
        {
            if (bytes.size() < sizeof(std::uint32_t) * 5 ||
                bytes.size() % sizeof(std::uint32_t) != 0)
            {
                return false;
            }

            std::uint32_t magic{};
            std::memcpy(&magic, bytes.data(), sizeof(magic));
            return magic == kSpirvMagic;
        }

        void removeBestEffort(const std::filesystem::path& path) noexcept
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } // namespace

    lux::cxx::expected<void, lux::asset::EAssetError>
    packSpirvAsset(
        const std::filesystem::path& source,
        const std::filesystem::path& target,
        const lux::asset::AssetInfo& asset_info
    ) noexcept
    {
        using lux::asset::AssetSerDeser;
        using lux::asset::EAssetError;
        using lux::asset::EAssetType;
        using lux::cxx::unexpected;

        if (asset_info.type != EAssetType::SHADER)
        {
            return unexpected(EAssetError::FILE_TYPE_ERROR);
        }

        std::ifstream input(source, std::ios::binary | std::ios::ate);
        if (!input)
        {
            return unexpected(EAssetError::FILE_OPEN_FAIL);
        }

        const auto end = input.tellg();
        if (end <= 0)
        {
            return unexpected(EAssetError::ABNORMAL_FILE_SIZE);
        }
        input.seekg(0, std::ios::beg);

        std::vector<std::byte> spirv(static_cast<std::size_t>(end));
        if (!input.read(
                reinterpret_cast<char*>(spirv.data()),
                static_cast<std::streamsize>(spirv.size())
            ))
        {
            return unexpected(EAssetError::READ_FILE_FAIL);
        }
        if (!hasSpirvHeader(spirv))
        {
            return unexpected(EAssetError::FILE_TYPE_ERROR);
        }

        lux::rdesc::ShaderInfo shader_info{};
        if (!reflectSpirv(spirv.data(), spirv.size(), shader_info))
        {
            return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        }
        const auto info_bytes = lux::rdesc::ShaderInfo::serialize(shader_info);
        if (info_bytes.empty())
        {
            return unexpected(EAssetError::ASSET_NO_INFO);
        }

        std::error_code directory_error;
        const auto parent = target.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, directory_error);
            if (directory_error)
            {
                return unexpected(EAssetError::UNKNOWN_FILESYSTEM_ERROR);
            }
        }

        auto header = AssetSerDeser::makeHeaderRaw<EAssetType::SHADER>(
            asset_info,
            info_bytes.size(),
            spirv.size()
        );
        auto temporary = target;
        temporary += ".tmp";

        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc
        );
        if (!output)
        {
            return unexpected(EAssetError::FILE_OPEN_FAIL);
        }
        output.write(
            reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size())
        );
        output.write(
            reinterpret_cast<const char*>(info_bytes.data()),
            static_cast<std::streamsize>(info_bytes.size())
        );
        output.write(
            reinterpret_cast<const char*>(spirv.data()),
            static_cast<std::streamsize>(spirv.size())
        );
        output.flush();
        output.close();
        if (!output.good())
        {
            removeBestEffort(temporary);
            return unexpected(EAssetError::WRITE_FILE_FAIL);
        }

        std::error_code rename_error;
        std::filesystem::rename(temporary, target, rename_error);
        if (rename_error)
        {
            removeBestEffort(temporary);
            return unexpected(EAssetError::WRITE_FILE_FAIL);
        }
        return {};
    }
} // namespace lux::toolchain
