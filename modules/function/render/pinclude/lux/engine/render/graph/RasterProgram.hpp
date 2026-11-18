#pragma once

#include <NRI.h>
#include <array>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/extensions/graph/Draw.hpp>
#include <memory>
#include <span>
#include <utility>

namespace lux::render::detail
{
    struct EnabledDeviceFeatures;

    struct PreparedShader final
    {
        explicit PreparedShader(rdesc::ShaderProgramArtifact value) noexcept
            : artifact(std::move(value)), digest(artifact.binaryDigest())
        {
        }

        const rdesc::ShaderProgramArtifact artifact;
        const std::size_t digest;

        [[nodiscard]] bool matches(const PreparedShader& other) const noexcept
        {
            return this == &other || (digest == other.digest && artifact == other.artifact);
        }
    };

    // Exact pipeline identity after layout relocation. Shader includes entry points and reflected ABI.
    // Device/table capacities are fixed by the Device; resource IDs/addresses are not PSO identity.
    struct RasterProgramKey final
    {
        std::shared_ptr<const PreparedShader> shader;
        GraphicsState state;
        std::vector<nri::Format> colors;
        nri::Format depth = nri::Format::UNKNOWN;
        std::uint32_t samples = 1;
        [[nodiscard]] bool matches(const RasterProgramKey& other) const noexcept
        {
            const bool same_state = samples == other.samples && depth == other.depth &&
                colors == other.colors && state == other.state;
            return same_state && shader->matches(*other.shader);
        }
    };

    // Any returned native handle belongs to the caller, including on failure.
    // The native layout is borrowed from the same cold placement used to relocate the shader.
    [[nodiscard]] RenderResult<void> createRasterProgram(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const nri::DeviceDesc& limits,
        const EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::PipelineLayout& layout,
        const RasterProgramKey& program,
        nri::Pipeline*& pipeline
    ) noexcept;
} // namespace lux::render::detail
