#pragma once

#include <cstdint>
#include <string_view>
#include <lux/cxx/core/StableNameId.hpp>
#include <lux/cxx/core/StrongId.hpp>

namespace lux::render
{
    struct RenderDataTypeTag;
    struct FeatureTypeTag;
    struct SceneCapabilityTag;
    struct RenderTargetSemanticTag;

    using RenderDataTypeId = cxx::StrongId<RenderDataTypeTag, std::uint64_t, 0>;
    using FeatureTypeId = cxx::StrongId<FeatureTypeTag, std::uint64_t, 0>;
    using SceneCapabilityId = cxx::StrongId<SceneCapabilityTag, std::uint64_t, 0>;
    using RenderTargetSemanticId = cxx::StrongId<RenderTargetSemanticTag, std::uint64_t, 0>;

    // Canonical names are exact UTF-8 bytes, without normalization. Zero is reserved.
    // Hash at composition time; retain the canonical name to detect collisions.
    [[nodiscard]] constexpr RenderDataTypeId renderDataTypeId(std::string_view name) noexcept
    {
        return RenderDataTypeId{name.empty() ? 0 : cxx::Fnv1a64::hash(name)};
    }

    [[nodiscard]] constexpr FeatureTypeId featureTypeId(std::string_view name) noexcept
    {
        return FeatureTypeId{name.empty() ? 0 : cxx::Fnv1a64::hash(name)};
    }

    [[nodiscard]] constexpr SceneCapabilityId sceneCapabilityId(std::string_view name) noexcept
    {
        return SceneCapabilityId{name.empty() ? 0 : cxx::Fnv1a64::hash(name)};
    }

    [[nodiscard]] constexpr RenderTargetSemanticId renderTargetSemanticId(std::string_view name) noexcept
    {
        return RenderTargetSemanticId{name.empty() ? 0 : cxx::Fnv1a64::hash(name)};
    }
}
