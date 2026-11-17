#include <algorithm>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>

namespace lux::render
{
    std::string featureKey(FeatureName type, std::string_view instance, std::string_view local) noexcept
    {
        return "feature:" + std::to_string(type.name().size()) + ":" + std::string(type.name()) + ":" +
               std::to_string(instance.size()) + ":" + std::string(instance) + ":" + std::string(local);
    }

    void FeatureBuildContext::require(StorageArgument argument) noexcept
    {
        arguments_.push_back(std::move(argument));
    }

    ResourceVersion FeatureBuildContext::resource(std::string_view key) const noexcept
    {
        const auto found = std::ranges::find(ports_, key, &ResolvedResource::key);
        return found == ports_.end() ? ResourceVersion{} : found->version;
    }

    RenderResult<void> FeatureBuildContext::publish(std::string_view key, ResourceVersion version) noexcept
    {
        const auto found = std::ranges::find(ports_, key, &ResolvedResource::key);
        if (found == ports_.end())
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        found->version = version;
        return {};
    }

} // namespace lux::render
