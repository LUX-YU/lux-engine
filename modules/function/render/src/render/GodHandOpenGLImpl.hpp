#pragma once
#include <lux-engine/function/graphic_api/opengl3/ShaderProgram.hpp>
#include <lux-engine/function/graphic_api/opengl3/VertexBufferObject.hpp>

#include "GodHandInterface.hpp"
#include <glad/glad.h>

namespace lux::engine::function
{
    class GodHandOpenGLImpl : public GodHandInterface
    {
    public:
        GodHandOpenGLImpl();

        void drawModel(const LuxModel& model, const Position& position) override;
    };
}
