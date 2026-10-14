#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/graph/Analysis.hpp>

int main()
{
    using namespace lux::render;
    RenderGraphBuilder builder;
    auto resource = builder.declareResource({"image", GraphImageDesc{640, 480}});
    if (!resource) return 1;
    auto pass = builder.addPass({"clear", EGraphPassKind::GRAPHICS,
        {{{*resource, 1}, {}, EGraphAccess::WRITE}}});
    if (!pass) return 2;
    TargetDesc target{640, 480, lux::rdesc::ETextureFormat::RGBA8_UNORM};
    if (!builder.exportOutput({{*resource, 1}, {}, target})) return 3;
    auto definition = std::move(builder).finish();
    if (!definition) return 4;
    auto analysis = analyze(*definition);
    if (!analysis || analysis->execution_order.size() != 1 || analysis->lifetimes[0].last != 1) return 5;
    return 0;
}
