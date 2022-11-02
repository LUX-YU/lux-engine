#pragma once
#include <Eigen/Eigen>
#include "lux-engine/resource/model/Model.hpp"
#include "lux-engine/platform/window/LuxWindow.hpp"
#include "lux-engine/function/graphic_api/opengl3/ShaderProgram.hpp"
#include "lux-engine/function/graphic_api/opengl3/VertexBufferObject.hpp"

namespace lux::engine::function
{
    using Position  = ::Eigen::Vector3f;
    using LuxWindow = ::lux::engine::platform::LuxWindow;
    using LuxModel  = ::lux::engine::resource::Model;

    class GodHandInterface;

    // If I want use different graphic api backend
    // I should design a RHI
    class GodHand
    {
    public:
        /**
         * @brief 
         * 
         * @return LUX_EXPORT 
         */
        LUX_EXPORT GodHand(LuxWindow&);

        ~GodHand();

        LUX_EXPORT void drawModel(const LuxModel& model, const Position& position);

    private:
        std::unique_ptr<GodHandInterface> _impl;
    };
} // namespace lux::engine::function
