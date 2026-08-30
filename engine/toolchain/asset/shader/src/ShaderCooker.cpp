#include <lux/engine/toolchain/asset/shader/ShaderCooker.hpp>

#include <lux/engine/toolchain/shader/SpirvReflection.hpp>

#include <cstring>
#include <new>
#include <utility>

namespace lux::toolchain
{
    namespace
    {
        inline constexpr std::uint32_t kSpirvMagic = 0x07230203U;

        [[nodiscard]] ShaderCookFailure failure(EShaderCookError code, std::size_t offset = 0U) noexcept
        {
            return ShaderCookFailure{code, offset};
        }

        [[nodiscard]] bool validSpirvHeader(lux::cxx::SharedBytes<> spirv) noexcept
        {
            if (spirv.size() < sizeof(std::uint32_t) * 5U || spirv.size() % sizeof(std::uint32_t) != 0U)
                return false;
            std::uint32_t magic{};
            std::memcpy(&magic, spirv.data(), sizeof(magic));
            return magic == kSpirvMagic;
        }
    } // namespace

    lux::cxx::expected<std::shared_ptr<const lux::asset::ShaderAsset>, ShaderCookFailure> cookShader(
        lux::asset::AssetInfo metadata,
        lux::cxx::SharedBytes<> spirv
    ) noexcept
    {
        if (metadata.id.isNull() || spirv.empty())
            return lux::cxx::unexpected(failure(EShaderCookError::INVALID_SOURCE));
        if (!validSpirvHeader(spirv))
            return lux::cxx::unexpected(failure(EShaderCookError::INVALID_SPIRV));
        try
        {
            lux::rdesc::ShaderInfo reflected{};
            if (!reflectSpirv(spirv.data(), spirv.size(), reflected))
                return lux::cxx::unexpected(failure(EShaderCookError::REFLECTION_FAILED));
            auto data = std::make_shared<const lux::asset::ShaderAssetData>(lux::asset::ShaderAssetData{
                lux::rdesc::Shader{spirv.data(), spirv.size()},
                std::move(reflected)
            });
            auto asset = lux::asset::ShaderAsset::create(std::move(metadata), std::move(data));
            if (!asset)
                return lux::cxx::unexpected(failure(EShaderCookError::INVALID_COOKED_SHADER));
            return *asset;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EShaderCookError::ALLOCATION_FAILURE));
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(EShaderCookError::REFLECTION_FAILED));
        }
    }
} // namespace lux::toolchain
