#pragma once

#include <lux/engine/render/graph/Work.hpp>
#include <lux/engine/render/graph/ConstantBlock.hpp>
#include <lux/engine/render/resource/material/Data.hpp>
#include <lux/engine/render/resource/mesh/Data.hpp>
#include <lux/engine/description/EmbeddedShader.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace lux::render
{
    struct MeshVertexLayout final
    {
        std::uint32_t vertex_stride;
        std::span<const GeometryAttribute> attributes;
    };
}

namespace lux::render::detail
{
    struct MeshViewLayer final
    {
        std::uint32_t index;
    };
}

template <> struct lux::render::ConstantBlockTraits<lux::render::detail::MeshViewLayer> final
{
    static lux::render::ConstantBlockLayout layout(lux::rdesc::EShaderTarget target) noexcept
    {
        using namespace lux::render;
        using Block = detail::MeshViewLayer;
        static constexpr std::array members{
            ConstantMemberLayout{"index", offsetof(Block, index), lux::rdesc::EDataScalar::UINT}
        };
        return {sizeof(Block), target == lux::rdesc::EShaderTarget::DXIL ? 16u : 4u, members};
    }
};

namespace lux::render::detail
{
    enum class EMeshRasterInput : std::uint8_t
    {
        TRANSFORMS,
        PROPERTIES,
        ALIVE,
        VIEWS,
        MEMBERS,
        INDIRECT_COMMANDS
    };

    inline constexpr auto kMeshRasterMemberInput = static_cast<std::size_t>(EMeshRasterInput::MEMBERS);
    inline constexpr auto kMeshRasterIndirectInput = static_cast<std::size_t>(EMeshRasterInput::INDIRECT_COMMANDS);
    inline constexpr std::array<std::string_view, kMeshRasterIndirectInput> kMeshRasterInputNames{
        "transforms", "properties", "alive", "views", "members"
    };

    using MeshVertexShaders = std::array<rdesc::EmbeddedShader, 2>;

    // Both windings use the merged interface and the same owned fragment stage.
    struct MeshMaterialShader final
    {
        rdesc::EShaderTarget target;
        rdesc::ShaderStageArtifact fragment;
        std::array<std::span<const std::uint32_t>, 2> vertex_words;
        rdesc::ShaderInfo interface;
        ECullMode cull_mode;

        [[nodiscard]] rdesc::ShaderProgramArtifact artifact(bool reflected) const noexcept;
    };

    // An unsupported mesh purpose leaves other uses of the material available.
    [[nodiscard]] std::unique_ptr<MeshMaterialShader>
    prepareMeshShader(const rdesc::MaterialProgramSet&, rdesc::MaterialShader&&, bool paged) noexcept;

    struct MeshRasterLayout final
    {
        GraphicsState graphics;
        std::vector<std::uint32_t> geometry_locations;
    };

    [[nodiscard]] RenderResult<MeshRasterLayout>
    meshRasterLayout(const MeshVertexLayout&, const MeshMaterialShader&) noexcept;

    // Shared geometric ports describe data, not material groups or pipeline counts.
    [[nodiscard]] std::vector<RasterPassTemplate::Input> meshRasterInputs(bool record_table);
} // namespace lux::render::detail
