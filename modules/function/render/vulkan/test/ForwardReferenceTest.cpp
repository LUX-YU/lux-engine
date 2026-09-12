#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RenderGraphCompiler.hpp>

#include <cstdio>
#include <limits>

namespace lux::render
{
    struct RenderGraphCompilerTestAccess
    {
        static void resolve(RGCompiledGraph& graph)
        {
            RenderGraphCompiler::resolveForwardReferences(graph);
        }
    };
}

namespace
{
    using namespace lux::render;

    enum class EAccess { READ, WRITE, READ_WRITE, INPUT };

    void access(RGBuilder& builder, RGResourceHandle texture, EAccess mode, const char* name)
    {
        auto pass = builder.addPass(name, ERGPassType::GRAPHICS);
        switch (mode)
        {
        case EAccess::READ: pass.read(texture); break;
        case EAccess::WRITE: pass.write(texture); break;
        case EAccess::READ_WRITE: pass.readWrite(texture); break;
        case EAccess::INPUT: pass.inputRead(texture, 0); break;
        }
    }

    bool checkRange(bool array, std::uint32_t layers, EAccess mode)
    {
        RGBuilder builder;
        const auto placeholder = builder.referenceTexture("atlas");
        access(builder, placeholder, mode, "automatic-before-import");
        access(builder, placeholder, mode, "explicit-one-layer");
        builder.graphInternal().passes.back().textures.back().range.layer_count = 1;
        access(builder, placeholder, mode, "explicit-subrange");
        auto& subrange = builder.graphInternal().passes.back().textures.back().range;
        subrange.base_array_layer = layers > 1 ? 1 : 0;
        subrange.layer_count = layers > 2 ? 2 : 1;

        RGTextureDescription description;
        description.dimension = array ? lux::rdesc::ETextureDimension::TEX_2D_ARRAY
                                      : lux::rdesc::ETextureDimension::TEX_2D;
        description.array_layers = layers;
        description.width = description.height = 32;
        description.format = lux::rdesc::ETextureFormat::D32_SFLOAT;
        const auto actual = builder.importTexture("atlas", description, {});
        access(builder, actual, mode, "automatic-after-import");

        RGCompiledGraph graph;
        graph.original_graph = std::move(builder).build();
        RenderGraphCompilerTestAccess::resolve(graph);
        const auto& passes = graph.original_graph.passes;
        bool correct = passes.size() == 4;
        for (const auto& pass : passes)
            correct &= pass.textures.front().resource.index == actual.index &&
                       pass.textures.front().range.layer_count != (std::numeric_limits<std::uint32_t>::max)();
        correct &= passes[0].textures[0].range.layer_count == layers;
        correct &= passes[1].textures[0].range.layer_count == 1;
        correct &= passes[2].textures[0].range.base_array_layer == (layers > 1 ? 1U : 0U);
        correct &= passes[2].textures[0].range.layer_count == (layers > 2 ? 2U : 1U);
        correct &= passes[3].textures[0].range.layer_count == layers;
        std::printf("forward-reference array=%u layers=%u access=%u automatic=%u known=%u explicit=%u/%u result=%u\n",
                    unsigned(array), layers, unsigned(mode), passes[0].textures[0].range.layer_count,
                    passes[3].textures[0].range.layer_count, passes[1].textures[0].range.layer_count,
                    passes[2].textures[0].range.layer_count, unsigned(correct));
        return correct;
    }
}

int main()
{
    unsigned cases{}, failures{};
    for (const auto mode : {EAccess::READ, EAccess::WRITE, EAccess::READ_WRITE, EAccess::INPUT})
    {
        failures += !checkRange(false, 1, mode);
        ++cases;
        for (const std::uint32_t layers : {1U, 4U, 7U})
        {
            failures += !checkRange(true, layers, mode);
            ++cases;
        }
    }
    std::printf("forward-reference protocol cases=%u failures=%u\n", cases, failures);
    return failures ? 1 : 0;
}
