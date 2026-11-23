#pragma once

#include <lux/cxx/reflection/runtime/Marker.hpp>
#include <lux/engine/render/effect/builtin/Lighting.hpp>
#include <lux/engine/render/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/capabilities/MeshShadows.hpp>
#include <lux/engine/render/capabilities/RasterSurface.hpp>
#include <lux/engine/scene/ViewComponent.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/Visual.hpp>

// This Editor-owned parser input chooses values without annotating lower layers.
// The declarations vanish during normal C++ compilation; the generated functions
// access the original fields directly and own no additional copy of the values.
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::ecs::Transform3D)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::scene::ViewComponent)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::scene::PerspectiveProjection)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::scene::OrthographicProjection)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::ecs::Mesh3D)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::ecs::MaterialComponent)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::ecs::Light3D)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::rdesc::MeshVisualDescription)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::rdesc::LightDescription)
LUX_REFLECT_EXTERNAL_ENUM(luxeditor, ::lux::rdesc::ELightType)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::render::RasterSurfaceConfiguration)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::render::MeshShadowsConfiguration)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::render::ClusterGrid)
LUX_REFLECT_EXTERNAL(luxeditor, ::lux::render::DeferredLightingConfiguration)
LUX_REFLECT_EXTERNAL_ENUM(luxeditor, ::lux::render::EClusterDepth)
