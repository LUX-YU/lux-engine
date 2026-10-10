#pragma once

#include <array>
#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/PassContract.hpp>
#include <span>
#include <string>
#include <vector>

namespace lux::toolchain
{
    enum class EShaderScalarType
    {
        FLOAT,
        SINT,
        UINT
    };

    struct ShaderLocationType
    {
        std::uint32_t location, component, width, components;
        EShaderScalarType scalar;
        bool flat, noperspective;
        bool operator==(const ShaderLocationType&) const noexcept = default;
    };

    struct PassStageInterface
    {
        std::uint32_t stage;
        std::vector<ShaderLocationType> inputs, outputs;
        std::vector<std::uint32_t> capabilities;
        std::array<std::uint32_t, 3> workgroup{};
        bool local_size_id{};
    };

    // Same SPIRV-Cross boundary as schema reconciliation. Numeric arrays/matrices expand to locations;
    // unsupported interface blocks/64-bit or dual-source interfaces fail explicitly.
    [[nodiscard]] cxx::expected<PassStageInterface, std::string> reflectPassInterface(
        std::span<const std::uint32_t> words
    ) noexcept;

    // Cold final placement supplied by the single native LayoutPlan authority.
    struct PassDescriptorLocation
    {
        std::uint32_t field_index, set, binding;
        bool operator==(const PassDescriptorLocation&) const noexcept = default;
    };

    // Cold-only boundary. SPIRV-Cross types remain private to the toolchain.
    [[nodiscard]] cxx::expected<void, std::string> validatePassSpirv(
        std::span<const std::uint32_t> words,
        const rdesc::PassShaderContract& contract,
        std::span<const PassDescriptorLocation> final_locations = {}
    ) noexcept;

    struct PassShaderModule
    {
        std::span<const std::uint32_t> words;
    };

    // Complete program validation uses the same canonical slots for shared stage resources.
    [[nodiscard]] cxx::expected<void, std::string> validatePassShaders(
        std::span<const PassShaderModule> modules,
        const rdesc::PassShaderContract& contract,
        std::uint32_t required_stages
    ) noexcept;
} // namespace lux::toolchain
