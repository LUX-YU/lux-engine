#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <string_view>

namespace lux::render
{
    struct FeatureBuildContext;

    // Synchronous cold contributions. Callbacks may append, never finish/move/retain the Builder.
    struct FeatureGraphOps final
    {
        using Build = RenderResult<void> (*)(const void*, std::string_view, FeatureBuildContext&) noexcept;

        Build build = nullptr;
    };

    // Arguments: canonical feature index, resource/group index, related index.
    inline constexpr auto kFeatureContract = error::errorId("lux.render.feature.contract");
    inline constexpr auto kFeatureDuplicate = error::errorId("lux.render.feature.duplicate");
    inline constexpr auto kFeatureMissingProvider = error::errorId("lux.render.feature.missing_provider");
    inline constexpr auto kFeatureResourceMismatch = error::errorId("lux.render.feature.resource_mismatch");
    inline constexpr auto kFeatureOwnership = error::errorId("lux.render.feature.ownership");
    inline constexpr auto kFeatureVersion = error::errorId("lux.render.feature.version");
    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor> renderFeatureErrorDescriptors() noexcept;
} // namespace lux::render
