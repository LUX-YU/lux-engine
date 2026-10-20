#include <algorithm>
#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/FeatureBuildContext.hpp>

namespace lux::render
{
    ResourceVersion FeatureBuildContext::resource(std::string_view key) const noexcept
    {
        const auto found = std::ranges::find(ports, key, &ResolvedResource::key);
        return found == ports.end() ? ResourceVersion{} : found->version;
    }

    RenderResult<void> FeatureBuildContext::publish(std::string_view key, ResourceVersion version) noexcept
    {
        const auto found = std::ranges::find(ports, key, &ResolvedResource::key);
        if (found == ports.end())
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        found->version = version;
        return {};
    }

} // namespace lux::render
