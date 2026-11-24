#pragma once

#include <lux/engine/render/resource/ResourceRequirement.hpp>

namespace lux::render
{
    inline constexpr ResourceNameView kInstanceResourceName{"lux.render.resource.instances"};
    inline constexpr ResourceRequirement kSceneMeshInstances{"mesh.instances", kInstanceResourceName, EResourceDomain::SCENE};
}
