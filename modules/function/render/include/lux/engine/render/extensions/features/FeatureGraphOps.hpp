#pragma once

#include <lux/cxx/core/Schema.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <string_view>
#include <type_traits>

namespace lux::render
{
    struct FeatureBuildContext;
    struct FeaturePorts;
    struct FeatureNameTag;
    using FeatureName = cxx::SchemaIdView<FeatureNameTag>;

    // Synchronous cold contributions. Callbacks may append, never finish/move/retain the Builder.
    struct FeatureGraphOps final
    {
        using Build = RenderResult<void> (*)(
            const void*, std::string_view, FeatureBuildContext&
        ) noexcept;
        using Describe = RenderResult<void> (*)(const void*, std::string_view, FeaturePorts&) noexcept;
        Describe describe_ports = nullptr;
        Build build = nullptr;

        template <class Feature> static constexpr FeatureGraphOps make() noexcept
        {
            return {
                [](const void* config, std::string_view key, FeaturePorts& ports) noexcept
                {
                    return Feature::Ports::describe(static_cast<const typename Feature::Config*>(config), key, ports);
                },
                [](const void* config, std::string_view key, FeatureBuildContext& context) noexcept
                {
                    const auto* typed = static_cast<const typename Feature::Config*>(config);
                    return Feature::build(typed, key, context);
                }
            };
        }

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
