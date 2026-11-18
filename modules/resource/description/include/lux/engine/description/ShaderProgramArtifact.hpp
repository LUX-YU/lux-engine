#pragma once

#include <functional>
#include <string_view>

#include <lux/engine/description/ShaderInfo.hpp>

namespace lux::rdesc
{
    enum class EShaderTarget : std::uint8_t
    {
        SPIRV,
        DXIL
    };

    struct ShaderStageArtifact final
    {
        EShaderType stage = EShaderType::UNDEFINED;
        std::string entry;
        // Both supported containers are word aligned. These are owned binary bytes, not portable IR.
        std::vector<std::uint32_t> words;
        bool operator==(const ShaderStageArtifact&) const noexcept = default;
    };

    struct ShaderProgramArtifact final
    {
        std::vector<ShaderStageArtifact> stages;
        ShaderInfo interface;
        EShaderTarget target = EShaderTarget::SPIRV;
        // Binary fingerprint for candidate selection. Reflected ABI still requires exact comparison.
        [[nodiscard]] std::size_t binaryDigest() const noexcept
        {
            std::size_t result = 0;
            const auto combine = [&]<class T>(const T& value) noexcept
            {
                result ^= std::hash<T>{}(value) + 0x9e3779b9u + (result << 6) + (result >> 2);
            };
            combine(target);
            combine(stages.size());
            for (const auto& stage : stages)
            {
                combine(stage.stage);
                combine(stage.entry);
                if (!stage.words.empty())
                {
                    const auto bytes = std::as_bytes(std::span(stage.words));
                    combine(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
                }
            }
            return result;
        }

        bool operator==(const ShaderProgramArtifact&) const noexcept = default;
    };

    // Container qualification only. Backend validation remains necessary before native pipeline creation.
    [[nodiscard]] inline bool validShaderContainer(EShaderTarget target, std::span<const std::uint32_t> words) noexcept
    {
        if (target == EShaderTarget::SPIRV)
        {
            return words.size() >= 5 && words[0] == 0x07230203U && words[1] != 0 && words[3] != 0 && words[4] == 0;
        }
        if (target != EShaderTarget::DXIL || words.size() < 8)
        {
            return false;
        }
        const bool is_invalid_header =
            words[0] != 0x43425844U || words[5] != 1 || words[6] != words.size_bytes() || words[7] > words.size() - 8;
        if (is_invalid_header)
        {
            return false;
        }
        bool has_dxil = false;
        for (std::uint32_t chunk = 0; chunk < words[7]; ++chunk)
        {
            const auto offset = words[8 + chunk];
            const bool is_invalid_offset =
                offset % 4 != 0 || offset / 4 < 8 + words[7] || offset / 4 > words.size() - 2;
            if (is_invalid_offset)
            {
                return false;
            }
            const auto position = offset / 4;
            if (words[position + 1] > words.size_bytes() - offset - 8)
            {
                return false;
            }
            has_dxil |= words[position] == 0x4c495844U;
        }
        return has_dxil;
    }
} // namespace lux::rdesc
