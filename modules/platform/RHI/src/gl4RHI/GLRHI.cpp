#pragma once
#include "lux/engine/RHI/gl4RHI/GLRHI.hpp"

namespace lux::engine::rhi::gl
{
	OpenGLRHI::OpenGLRHI()
	{

	}

	EGraphAPITypes OpenGLRHI::type() const
	{
		return EGraphAPITypes::OpenGL4;
	}
}
