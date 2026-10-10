#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Identity.hpp>

namespace lux::render
{
    // Borrowed assembly records, not registered/live objects. Names and spans
    // must outlive every reader; a plugin owner must pin its storage accordingly.
    // Layout describes a native value; it does not define a wire encoding.
    struct RenderDataDescriptor final
    {
        RenderDataTypeId id;
        std::string_view canonical_name;
        std::uint32_t wire_version{};
        std::uint32_t layout_version{};
        std::uint32_t size{};
        std::uint32_t alignment{};
    };

    struct SceneCapabilityDescriptor final
    {
        SceneCapabilityId id;
        std::string_view canonical_name;
        std::uint32_t contract_version{};
    };

    struct FeatureDescriptor final
    {
        FeatureTypeId id;
        std::string_view canonical_name;
        std::uint32_t contract_version{};
        std::span<const SceneCapabilityId> provides;
        std::span<const SceneCapabilityId> required_capabilities;
    };

    [[nodiscard]] RenderResult<void> validateDescriptor(const RenderDataDescriptor& descriptor) noexcept;
    [[nodiscard]] RenderResult<void> validateDescriptor(const SceneCapabilityDescriptor& descriptor) noexcept;
    [[nodiscard]] RenderResult<void> validateDescriptor(const FeatureDescriptor& descriptor) noexcept;

    // Compares supplied metadata; this does not validate either record. A later
    // registration must call both checks. Different IDs are a mismatch.
    // No registry, route, lane, reply or executable feature metadata lives here.
    [[nodiscard]] RenderResult<void>
    validateCompatible(const RenderDataDescriptor& existing, const RenderDataDescriptor& candidate) noexcept;
}
