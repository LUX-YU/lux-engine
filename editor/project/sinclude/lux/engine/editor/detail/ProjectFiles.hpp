#pragma once
#include <lux/engine/editor/ProjectManifest.hpp>
#include <span>

namespace lux::editor::detail
{
    [[nodiscard]] ProjectResult<std::vector<std::byte>> readProjectBytes(
        const std::filesystem::path&,
        std::size_t limit,
        std::stop_token
    ) noexcept;
    [[nodiscard]] ProjectResult<void> writeProjectBytesAtomic(
        const std::filesystem::path&,
        std::span<const std::byte>,
        EProjectWrite,
        std::stop_token
    ) noexcept;
} // namespace lux::editor::detail
