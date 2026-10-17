#pragma once

#include "../../native/vulkan/test/SceneFixture.hpp"
#include "shaders/Camera.hpp"
#include <lux/engine/render/data/Camera.hpp>

namespace camera_fixture
{
    inline lux::render::CameraState oracle(std::array<float, 4> value) noexcept
    {
        lux::render::CameraState camera;
        std::copy(value.begin(), value.end(), camera.view.begin());
        return camera;
    }

    inline std::vector<lux::render::PassProgram> programs(native_fixture::Config config, std::string_view instance)
    {
        using namespace lux::render;
        auto result = native_fixture::programs(config, instance);
        if (config.graphics)
        {
            auto& draw = result.front();
            draw.shader.stages[1].words.assign(fragment.begin(), fragment.end());
            draw.shader.interface.parameters = cameraParameterBlock();
            draw.parameter_values.resize(128);
            draw.context_parameters = cameraParameterBlock();
            draw.parameters = {{"view", "", "view", EParameterSource::CONTEXT},
                {"projection", "", "projection", EParameterSource::CONTEXT}};
        }
        return result;
    }
}
