#include <array>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/graph/compile/GraphDefinition.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>

namespace lux::render
{
    std::span<const error::ErrorDescriptor> renderCoreErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.core.invalid_target_extent",
             "Target width {0} and height {1} must be nonzero and supported",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.core.invalid_target_format",
             "Target format {0} must describe an uncompressed color attachment",
             error::ERecovery::PERMANENT,
             {error::EArgument::HEX}},
            {"lux.render.core.invalid_target_samples",
             "Target sample count {0} must be a power of two from 1 through 64",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED}},
            {"lux.render.core.invalid_target_kind",
             "Unknown target kind {0}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED}},
            {"lux.render.core.invalid_viewport",
             "Raster viewport coordinates and depth ranges must be valid",
             error::ERecovery::PERMANENT,
             {}},
            {"lux.render.runtime.outcome_unknown", "Accepted side effects have an unknown outcome"},
            {"lux.render.data.invalid_config", "Invalid configuration"},
            {"lux.render.data.invalid_effect_arguments",
             "Effect node {0} argument error {1} at text offset {2}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.capability", "Invalid capability declaration"},
            {"lux.render.capability.conflict", "Conflicting declarations for the same capability type"},
            {"lux.render.capability.cycle", "Effect Graph contains a cycle"},
            {"lux.render.capability.in_use", "Capability implementation or dependency is still in use"}
        };
        return descriptors;
    }

    std::span<const std::span<const error::ErrorDescriptor>> renderErrorDescriptorTables() noexcept
    {
        static const std::array tables{
            renderCoreErrorDescriptors(),
            renderCommandErrorDescriptors(),
            renderRuntimeErrorDescriptors(),
            renderResourceErrorDescriptors(),
            renderGraphErrorDescriptors(),
            deviceErrorDescriptors()
        };
        return tables;
    }
} // namespace lux::render
