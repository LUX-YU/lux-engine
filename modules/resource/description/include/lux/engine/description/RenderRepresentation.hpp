#pragma once

#include <lux/engine/core/extension_abi/StableId.hpp>

#include <string>
#include <string_view>

namespace lux::rdesc
{
    struct RenderRepresentationIdTag final {};
    struct TextureSamplingRepresentationIdTag final {};

    using RenderRepresentationIdView = lux::cxx::StableNameIdView<RenderRepresentationIdTag>;
    using RenderRepresentationId = lux::cxx::StableNameId<RenderRepresentationIdTag>;
    using TextureSamplingRepresentationIdView = lux::cxx::StableNameIdView<TextureSamplingRepresentationIdTag>;
    using TextureSamplingRepresentationId = lux::cxx::StableNameId<TextureSamplingRepresentationIdTag>;

    [[nodiscard]] constexpr RenderRepresentationIdView
    renderRepresentationId(std::string_view name) noexcept
    {
        return RenderRepresentationIdView{name};
    }

    [[nodiscard]] constexpr TextureSamplingRepresentationIdView
    textureSamplingRepresentationId(std::string_view name) noexcept
    {
        return TextureSamplingRepresentationIdView{name};
    }

    inline constexpr auto kClassicMeshRepresentation =
        renderRepresentationId("lux.render.geometry.classic_mesh");
    inline constexpr auto kBindlessTextureSamplingRepresentation =
        textureSamplingRepresentationId("lux.render.texture.bindless");

    [[nodiscard]] inline RenderRepresentationId ownRenderRepresentationId(
        RenderRepresentationIdView id)
    {
        return RenderRepresentationId{id.name()};
    }

    [[nodiscard]] inline TextureSamplingRepresentationId
    ownTextureSamplingRepresentationId(
        TextureSamplingRepresentationIdView id)
    {
        return TextureSamplingRepresentationId{id.name()};
    }
} // namespace lux::rdesc
