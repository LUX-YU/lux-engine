#include <array>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/render/extensions/graph/RenderGraph.hpp>
#include <lux/engine/render/extensions/storage/StorageDescriptor.hpp>

namespace lux::render
{
    std::span<const std::span<const error::ErrorDescriptor>> renderErrorDescriptorTables() noexcept
    {
        static const std::array tables{
            renderCoreErrorDescriptors(),
            renderCommandErrorDescriptors(),
            renderRuntimeErrorDescriptors(),
            renderResourceErrorDescriptors(),
            renderFeatureErrorDescriptors(),
            renderGraphErrorDescriptors(),
            deviceErrorDescriptors()
        };
        return tables;
    }
} // namespace lux::render
