#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <lux/engine/render/resource/Storage.hpp>
#include <algorithm>

namespace lux::render
{
    struct ShaderInterface final
    {
        std::vector<ShaderBinding> bindings;
        std::uint32_t push_bytes = 16;
        bool operator==(const ShaderInterface&) const noexcept = default;
    };
    // Cooked SPIR-V and exact entry-point ABI for the current real graphics path.
    // Owned bytes are the identity; no hash-only equality, compiler or parser in this header.
    struct ShaderArtifact final
    {
        std::vector<std::uint32_t> vertex;
        std::vector<std::uint32_t> fragment;
        ShaderInterface interface;
        bool operator==(const ShaderArtifact&) const noexcept = default;
    };

    // Cold compiler input, one binding per live Pass. It is the only Shader binding authority.
    // Feature registration uses a local name; assembly's caller expands it with featureKey().
    struct PassProgram final
    {
        std::string pass_key;
        ShaderArtifact shader; // empty for transfer-only commands
        std::array<float, 4> clear_color{0, 0, 0, 1};
        std::uint32_t vertex_count = 3;
        std::vector<StorageArgument> arguments;
    };

    [[nodiscard]] inline RenderResult<std::vector<StorageArgument>>
    programArguments(std::span<const PassProgram> programs) noexcept
    {
        std::vector<StorageArgument> result;
        for (const auto& program : programs)
            for (const auto& argument : program.arguments)
            {
                const auto found = std::ranges::find(result, argument.resource_key, &StorageArgument::resource_key);
                if (found == result.end()) result.push_back(argument);
                else if (!std::ranges::equal(found->bytes.view(), argument.bytes.view()))
                    return cxx::unexpected(RenderError{kStorageMismatch});
            }
        return result;
    }
}
