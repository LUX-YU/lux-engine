#include <lux/engine/render/graph/Bindings.hpp>

#if defined(LUX_PROBE_VULKAN)
#include <vulkan/vulkan.h>
#elif defined(LUX_PROBE_SCENE)
#include <lux/engine/scene/SceneSystem.hpp>
#elif defined(LUX_PROBE_RUNTIME)
#include <lux/engine/render/runtime/RenderRuntime.hpp>
#elif defined(LUX_PROBE_TRANSPORT)
#include <lux/engine/render/transport/Transport.hpp>
#elif defined(LUX_PROBE_LEGACY)
#include <lux/engine/function/render/graph/DependencyAnalyzer.hpp>
#elif defined(LUX_PROBE_EDITOR)
#include <lux/engine/editor/LuxEngine.hpp>
#elif defined(LUX_PROBE_FEATURE)
#include <lux/engine/render/features/Feature.hpp>
#endif

int main()
{
#if defined(LUX_PROBE_ID)
    lux::render::GraphPassId pass = lux::render::GraphResourceId{1};
    return static_cast<int>(pass.value());
#elif defined(LUX_PROBE_TEMPORARY)
    auto definition = lux::render::RenderGraphDefinition::create({}, {});
    auto bindings = lux::render::FrameGraphBindings::create(
        *lux::render::CompiledGraphPlan::compile(*definition), {}, {}
    );
    return bindings ? 0 : 1;
#else
    auto definition = lux::render::RenderGraphDefinition::create({}, {});
    return definition ? 0 : 1;
#endif
}
