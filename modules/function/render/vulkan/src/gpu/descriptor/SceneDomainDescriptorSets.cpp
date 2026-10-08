#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>

namespace lux::render
{
    SceneDomainDescriptorSets::CreateResult
    SceneDomainDescriptorSets::create(
        SceneDescriptorArena& arena, const GeneralDescriptorSetLayout& layouts, uint32_t slices
    ) noexcept
    {
        const bool is_invalid_frames = slices == 0 || slices > kMaxFramesInFlight;
        if (is_invalid_frames)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        for (const auto domain : kPerSceneDomains)
        {
            if (!layouts.getDomainLayout(domain))
            {
                return renderFailure<err::internal::InvalidArgument>();
            }
        }
        Sets sets;
        for (const auto domain : kPerSceneDomains)
        {
            auto& handles = sets[static_cast<std::size_t>(domain)];
            handles.reserve(slices);
            for (uint32_t i = 0; i < slices; ++i)
            {
                auto allocated = arena.allocate(layouts.getDomainLayout(domain));
                if (!allocated)
                {
                    return lux::cxx::unexpected(allocated.error());
                }
                handles.push_back(*allocated);
            }
        }
        return std::unique_ptr<SceneDomainDescriptorSets>(new SceneDomainDescriptorSets(std::move(sets)));
    }

    SceneDomainDescriptorSets::SceneDomainDescriptorSets(Sets&& sets) noexcept : sets_(std::move(sets)) {}

} // namespace lux::render
