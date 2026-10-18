#pragma once

#include "../../native/vulkan/test/SceneFixture.hpp"
#include "shaders/Camera.hpp"
#include <lux/engine/render/features/MeshFeature.hpp>
#include <lux/engine/render/data/Payload.hpp>

namespace camera_fixture
{
    inline lux::render::MeshViewParameters oracle(std::array<float, 4> value) noexcept
    {
        lux::render::MeshViewParameters camera;
        std::copy(value.begin(), value.end(), camera.view.begin());
        return camera;
    }

    inline lux::render::RenderPayload payload(const lux::render::MeshViewParameters& values) noexcept
    {
        return lux::render::RenderPayload::copyOf(lux::render::meshViewParameterBytes(values));
    }

    inline std::vector<lux::render::PassProgram> programs(native_fixture::Config config, std::string_view instance)
    {
        using namespace lux::render;
        auto result = native_fixture::programs(config, instance);
        if (config.graphics)
        {
            auto& draw = result.front();
            draw.shader.stages[1].words.assign(fragment.begin(), fragment.end());
            draw.shader.interface.parameters = meshViewParameterBlock();
            draw.parameter_values.resize(128);
            draw.context_parameters = meshViewParameterBlock();
            draw.parameters = {{"view", "", "view", EParameterSource::CONTEXT},
                {"projection", "", "projection", EParameterSource::CONTEXT}};
        }
        return result;
    }
}
