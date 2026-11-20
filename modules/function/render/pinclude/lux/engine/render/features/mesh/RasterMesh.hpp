#pragma once

#include <lux/engine/render/extensions/graph/Work.hpp>
#include <lux/engine/render/resources/material/Data.hpp>
#include <lux/engine/render/resources/mesh/Data.hpp>
#include <array>
#include <cstdint>
#include <string_view>

namespace lux::render::detail
{
    enum class EMeshRasterInput : std::uint8_t
    {
        TRANSFORMS,
        PROPERTIES,
        ALIVE,
        MEMBERS,
        INDIRECT_COMMANDS
    };

    inline constexpr auto kMeshRasterMemberInput = static_cast<std::size_t>(EMeshRasterInput::MEMBERS);
    inline constexpr auto kMeshRasterIndirectInput = static_cast<std::size_t>(EMeshRasterInput::INDIRECT_COMMANDS);
    inline constexpr std::array<std::string_view, kMeshRasterIndirectInput> kMeshRasterInputNames{
        "transforms", "properties", "alive", "members"
    };

    // Immutable material/geometry preparation. These functions do not inspect Storage, Graph,
    // resource identities or a View and can execute on the shared CPU scheduler.
    [[nodiscard]] RenderResult<rdesc::ShaderProgramArtifact>
    buildMeshShader(const rdesc::MaterialProgramSet&, rdesc::EMaterialPurpose, bool paged, bool reflected) noexcept;

    [[nodiscard]] RenderResult<GraphicsState> meshGraphicsState(
        std::uint32_t vertex_stride,
        std::span<const MeshAttribute> attributes,
        const rdesc::MaterialProgramSet&,
        const rdesc::ShaderInfo&,
        bool front_counter_clockwise,
        bool reflected
    ) noexcept;

    // Shared geometric ports describe data, not material groups or pipeline counts.
    [[nodiscard]] std::vector<RasterPassTemplate::Input> meshRasterInputs();
} // namespace lux::render::detail
