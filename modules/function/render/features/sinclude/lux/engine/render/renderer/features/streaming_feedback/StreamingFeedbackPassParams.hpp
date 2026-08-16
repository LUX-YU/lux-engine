#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>

#include <vulkan/vulkan.h>

namespace lux::render
{
    struct LUX_PASS_SCALARS() StreamingFeedbackScalars
    {
        float color_r{0.18f};
        float color_g{0.72f};
        float color_b{1.0f};
        float intensity{0.72f};
        float tile_size{18.0f};
        float speed{1.6f};
        float time_seconds{0.0f};
        float pattern{0.0f};
    };

    struct LUX_PASS_PARAMS() StreamingFeedbackPassParams
    {
        LUX_RESOURCE(role=read, glsl=uMask)  RGResourceHandle mask{};
        LUX_RESOURCE(role=sampler, for=mask) VkSampler        mask_sampler{VK_NULL_HANDLE};
        LUX_RESOURCE(role=write)             RGResourceHandle color_out{};

        StreamingFeedbackScalars scalars{};
    };
}
