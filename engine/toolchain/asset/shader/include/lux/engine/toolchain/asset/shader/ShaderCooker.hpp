#pragma once

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/shader/ShaderAsset.hpp>
#include <lux/engine/toolchain/asset/shader/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::toolchain
{
    enum class EShaderCookError : std::uint8_t
    {
        INVALID_SOURCE,
        INVALID_SPIRV,
        REFLECTION_FAILED,
        ALLOCATION_FAILURE,
        INVALID_COOKED_SHADER,
    };

    struct ShaderCookFailure final
    {
        EShaderCookError code{EShaderCookError::INVALID_SOURCE};
        std::size_t offset{};
    };

    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_SHADER_ASSET_PUBLIC lux::cxx::expected<
        std::shared_ptr<const lux::asset::ShaderAsset>,
        ShaderCookFailure
    > cookShader(
        lux::asset::AssetInfo metadata,
        lux::cxx::SharedBytes<> spirv
    ) noexcept;
} // namespace lux::toolchain
