#include <lux/engine/render/graph/RenderGraphCompiler.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <cassert>
#include <cstdio>

namespace lux::render
{
    // Non-installed test access to the actual compiler implementation exported by render_vulkan.
    struct RenderGraphCompilerTestAccess
    {
        static void resolve(RGCompiledGraph &graph)
        {
            RenderGraphCompiler::resolveForwardReferences(graph);
        }
        static void build(RGCompiledGraph &graph)
        {
            RenderGraphCompiler::buildBarrierProgram(graph);
        }
    };
} // namespace lux::render
int main()
{
    using namespace lux::render;
    unsigned cases{};
    for (const auto aspect : {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT})
    {
        for (unsigned mode = 0; mode != 4; ++mode)
        {
            RGCompiledGraph graph;
            graph.original_graph.resources.resize(1);
            auto &resource = graph.original_graph.resources[0];
            if (mode != 3)
            {
                resource.import_info = std::make_unique<RGImportedResourceInfo>();
                if (mode != 2)
                    resource.import_info->slot =
                        aspect == VK_IMAGE_ASPECT_COLOR_BIT ? TargetSlot::SCENE_COLOR : TargetSlot::SCENE_DEPTH;
                resource.import_info->preserve_content = mode == 1;
            }
            const VkImageMemoryBarrier2 initial{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .subresourceRange = {static_cast<VkImageAspectFlags>(aspect), 0, 1, 0, 1}};
            auto later = initial;
            later.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            later.srcStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
            later.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            later.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            graph.compiled_passes.resize(2);
            graph.execution_order = {0, 1};
            graph.compiled_passes[0].sync.prebuilt_image_barriers = {initial, initial};
            // Include a real sentinel patch index to exercise the resource bounds check.
            graph.compiled_passes[0].sync.image_patch_resource_idx = {0, RGCompiledGraph::kInvalidSlotIdx};
            graph.compiled_passes[1].sync.prebuilt_image_barriers = {later};
            graph.compiled_passes[1].sync.image_patch_resource_idx = {0};
            graph.imported_final_state_lut = {mode == 3 ? RGCompiledGraph::kInvalidSlotIdx : 0};
            graph.imported_final_states.push_back({0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            graph.prebuilt_final_barriers = {later};
            graph.final_patch_resource_idx = {0};
            RenderGraphCompilerTestAccess::build(graph);
            assert(graph.barrier_program);
            const auto &first = graph.barrier_program->first_view_barriers;
            const auto &next = graph.barrier_program->subsequent_view_barriers;
            const bool shared_or_preserved = mode == 1 || mode == 2;
            assert(first.size() == 2 && next.size() == 2);
            assert(first[0].image_barriers[0].oldLayout == initial.oldLayout);
            assert(next[0].image_barriers[0].oldLayout ==
                   (shared_or_preserved ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : initial.oldLayout));
            assert(next[0].image_barriers[0].srcStageMask ==
                   (shared_or_preserved ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : initial.srcStageMask));
            assert(next[0].image_barriers[0].srcAccessMask ==
                   (shared_or_preserved ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : initial.srcAccessMask));
            assert(next[0].image_src_is_final_state[0] == shared_or_preserved);
            assert(next[0].image_barriers[1].oldLayout == initial.oldLayout);
            assert(next[0].image_src_is_final_state[1] == 0);
            assert(next[1].image_barriers[0].oldLayout == later.oldLayout);
            assert(next[1].image_barriers[0].srcAccessMask == later.srcAccessMask);
            assert(next[1].image_src_is_final_state[0] == 0);
            assert(graph.barrier_program->final_barriers[0].image_barriers[0].newLayout == later.newLayout);
            ++cases;
        }
    }
    bool ranges_correct = true;
    for (const std::uint32_t layers : {1U, 4U})
    {
        RGBuilder builder;
        const auto forward = builder.referenceTexture("shared-array");
        builder.addPass("automatic-forward-read", ERGPassType::GRAPHICS).read(forward);
        builder.addPass("explicit-single-layer-read", ERGPassType::GRAPHICS).read(forward);
        builder.graphInternal().passes.back().textures.back().range.layer_count = 1;
        RGTextureDescription texture;
        texture.dimension = lux::rdesc::ETextureDimension::TEX_2D_ARRAY;
        texture.array_layers = layers;
        texture.width = texture.height = 32;
        texture.format = lux::rdesc::ETextureFormat::D32_SFLOAT;
        const auto actual = builder.importTexture("shared-array", texture, {});
        builder.addPass("known-array-read", ERGPassType::GRAPHICS).read(actual);
        RGCompiledGraph graph;
        graph.original_graph = std::move(builder).build();
        RenderGraphCompilerTestAccess::resolve(graph);
        const auto &passes = graph.original_graph.passes;
        const bool correct = passes[0].textures[0].resource.index == actual.index &&
                             passes[0].textures[0].range.layer_count == layers &&
                             passes[1].textures[0].range.layer_count == 1 &&
                             passes[2].textures[0].range.layer_count == layers;
        std::printf("forward array layers=%u automatic=%u explicit=%u known=%u correct=%u\n", layers,
                    passes[0].textures[0].range.layer_count, passes[1].textures[0].range.layer_count,
                    passes[2].textures[0].range.layer_count, correct);
        ranges_correct &= correct;
    }
    if (!ranges_correct)
    {
        std::fputs("barrier protocol FAIL: forward array range; original eight barrier cases passed\n", stderr);
        return 1;
    }
    std::printf("barrier program PASS core_cases=%u forward_array_cases=2 explicit ranges retained\n", cases);
}
