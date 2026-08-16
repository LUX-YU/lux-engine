#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/asset/Asset.hpp>

#include <filesystem>

namespace lux::toolchain
{
    // Convert raw SPIR-V into the cooked runtime SHADER asset format. This is
    // deliberately a toolchain adapter: Runtime only decodes the resulting
    // ShaderInfo and bytecode and never links spirv-cross.
    [[nodiscard]] lux::cxx::expected<void, lux::asset::EAssetError>
    packSpirvAsset(
        const std::filesystem::path& source,
        const std::filesystem::path& target,
        const lux::asset::AssetInfo& asset_info
    ) noexcept;
} // namespace lux::toolchain
