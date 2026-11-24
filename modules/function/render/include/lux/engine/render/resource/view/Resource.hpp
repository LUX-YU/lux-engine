#pragma once

#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/ResourceProjection.hpp>
#include <lux/engine/render/resource/view/Parameters.hpp>

namespace lux::render
{
    struct ViewParameterResource final
    {
        static constexpr std::string_view gpu_binding = "scene_view";
        [[nodiscard]] static ResourceProjection projection() noexcept
        {
            constexpr GraphBufferRange row{0, sizeof(ViewGpuParameters)};
            return {
                .description = GraphBufferDesc{row.size}, .initial_ranges = {row},
                .initial_use = EGraphUse::SHADER_BUFFER_READ, .read_use = EGraphUse::STORAGE_READ, .job_local = true
            };
        }

        using PerFrameData = std::tuple<ViewGpuParameters>;
        using Config = void;

        static inline const ResourceDefinition descriptor{
            kViewParameterResourceName,
            {},
            {},
            {},
            "Complete view parameters supplied by the application.",
            EResourceDomain::VIEW,
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };
} // namespace lux::render
