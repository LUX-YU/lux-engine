#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/graph/Builder.hpp>
#if defined(LUX_MISSING)
#include "Tonemap.hpp"
#else
#include "Tonemap.pass.hpp"
#endif

using namespace lux::render;

int main()
{
#if defined(LUX_TEXTURE_BUFFER)
    SampledTexture input;
    input.texture = GraphBuffer{1};
#elif defined(LUX_UNMARKED)
    struct Params
    {
        float exposure;
    };

    static_assert(GraphPassParameters<Params>, "F1 requires generated semantic Schema");
#elif defined(LUX_MISSING)
    static_assert(GraphPassParameters<Tonemap>, "F1 requires generated metadata include");
#elif defined(LUX_FINISH_BORROW)
    RenderGraphBuilder graph;
    auto definition = graph.finish();
#elif defined(LUX_RAW_DEFINITION)
    auto definition = RenderGraphDefinition::create({}, {});
#endif
}
