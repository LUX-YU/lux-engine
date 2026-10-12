#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/DataLayout.hpp>
#include <span>
#include <string>

namespace lux::toolchain
{
    // Cold validation of a generated record stored as block member `record`.
    [[nodiscard]] cxx::expected<void, std::string> validateDataSpirv(
        std::span<const std::uint32_t> words,
        const rdesc::DataGpuLayout& layout,
        std::string_view resource_name,
        bool runtime_array = false
    ) noexcept;
} // namespace lux::toolchain
