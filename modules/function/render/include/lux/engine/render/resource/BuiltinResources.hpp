#pragma once

#include <lux/engine/render/resource/ResourceDefinition.hpp>

namespace lux::render
{
    inline constexpr ResourceNameView kMeshResourceName{"lux.render.resource.mesh"};
    inline constexpr ResourceNameView kTextureResourceName{"lux.render.resource.texture"};
    inline constexpr ResourceNameView kMaterialResourceName{"lux.render.resource.material"};
    inline constexpr ResourceNameView kInstanceResourceName{"lux.render.resource.instances"};
    inline constexpr ResourceNameView kLightResourceName{"lux.render.resource.lights"};
    inline constexpr ResourceNameView kViewParameterResourceName{"lux.render.resource.view_parameters"};
    inline constexpr ResourceNameView kTransformResourceName{"lux.render.resource.transforms"};
} // namespace lux::render
