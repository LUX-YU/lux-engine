#pragma once
#include <lux/engine/editor/AssetImporter.hpp>
#include <lux/engine/toolchain/asset/model/ModelCookConfiguration.hpp>

namespace lux::editor
{
    struct ModelImporter final
    {
        using Configuration = toolchain::ModelCookConfiguration;
        static constexpr std::string_view name = "lux.import.model";
        static constexpr std::uint32_t version = 1;
        [[nodiscard]] static AssetImportResult<std::vector<std::byte>> encode(const Configuration&) noexcept;
        [[nodiscard]] static AssetImportResult<Configuration> decode(std::span<const std::byte>) noexcept;
        [[nodiscard]] static exec::task<AssetImportResult<AssetImportProduct>> cook(
            execution::ExecutionRuntime&,
            AssetImportSource,
            Configuration,
            execution::TaskReporter
        ) noexcept;
    };
} // namespace lux::editor
