#pragma once

#include <lux/engine/render/resource/ResourceRequirement.hpp>

#include <cstdint>

namespace lux::render
{
    inline constexpr ResourceNameView kTransformResourceName{"lux.render.resource.transforms"};
    inline constexpr ResourceRequirement kSceneTransforms{
        .binding = "mesh.transforms", .type = kTransformResourceName, .domain = EResourceDomain::SCENE, .scope_default = true
    };

    enum class ETransformGraphProjection : std::uint8_t
    {
        RECORDS,
        PAGE_ADDRESSES
    };

    inline constexpr std::uint32_t kTransformInitialPageAddresses = 8;

    [[nodiscard]] constexpr ETransformGraphProjection transformGraphProjection(bool paged) noexcept
    {
        return paged ? ETransformGraphProjection::PAGE_ADDRESSES : ETransformGraphProjection::RECORDS;
    }

    struct TransformBinding final
    {
        bool operator==(const TransformBinding&) const noexcept = default;
    };
} // namespace lux::render
