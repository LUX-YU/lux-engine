#include "Shader.hpp"
#include "Native.hpp"

#include <spirv-tools/libspirv.h>
#include <spirv_cross/spirv_cross_c.h>
#include <cstring>
#include <exception>
#include <memory>

namespace lux::render::vulkan
{
    namespace
    {
        bool check(spvc_result result) noexcept
        {
            if (result == SPVC_ERROR_OUT_OF_MEMORY) std::terminate();
            return result == SPVC_SUCCESS;
        }

        bool float4(spvc_type type) noexcept
        {
            return spvc_type_get_basetype(type) == SPVC_BASETYPE_FP32 &&
                spvc_type_get_vector_size(type) == 4 && spvc_type_get_columns(type) == 1 &&
                spvc_type_get_num_array_dimensions(type) == 0;
        }

        bool validateStage(std::span<const std::uint32_t> words, SpvExecutionModel stage, std::uint32_t abi) noexcept
        {
            // Use the existing SPIR-V tools, not another shader parser or handwritten instruction decoder.
            const auto validation = spvContextCreate(SPV_ENV_VULKAN_1_3);
            if (!validation) std::terminate();
            const auto valid = spvValidateBinary(validation, words.data(), words.size(), nullptr);
            spvContextDestroy(validation);
            if (valid == SPV_ERROR_OUT_OF_MEMORY) std::terminate();
            if (valid != SPV_SUCCESS) return false;
            spvc_context raw{};
            if (!check(spvc_context_create(&raw))) return false;
            const std::unique_ptr<spvc_context_s, decltype(&spvc_context_destroy)> context(raw, spvc_context_destroy);
            spvc_parsed_ir ir{};
            spvc_compiler compiler{};
            spvc_resources resources{};
            if (!check(spvc_context_parse_spirv(raw, words.data(), words.size(), &ir))) return false;
            if (!check(spvc_context_create_compiler(raw, SPVC_BACKEND_NONE, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                &compiler))) return false;
            const SpvCapability* capabilities{};
            std::size_t capability_count = 0;
            if (!check(spvc_compiler_get_declared_capabilities(compiler, &capabilities, &capability_count))) return false;
            for (std::size_t i = 0; i < capability_count; ++i)
            {
                const auto capability = capabilities[i];
                const bool sampled = abi == 2 && stage == SpvExecutionModelFragment &&
                    (capability == SpvCapabilityRuntimeDescriptorArray || capability == SpvCapabilityShaderNonUniform ||
                     capability == SpvCapabilitySampledImageArrayNonUniformIndexing ||
                     capability == SpvCapabilitySampledImageArrayDynamicIndexing || capability == SpvCapabilityImageQuery);
                if (capability != SpvCapabilityShader && !sampled) return false;
            }
            const spvc_entry_point* entries{};
            std::size_t count = 0;
            if (!check(spvc_compiler_get_entry_points(compiler, &entries, &count))) return false;
            const bool is_entry_mismatch = count != 1 || entries[0].execution_model != stage ||
                std::strcmp(entries[0].name, "main") != 0;
            if (is_entry_mismatch) return false;
            if (!check(spvc_compiler_create_shader_resources(compiler, &resources))) return false;
            for (unsigned kind = SPVC_RESOURCE_TYPE_UNIFORM_BUFFER; kind <= SPVC_RESOURCE_TYPE_SHADER_RECORD_BUFFER;
                ++kind)
            {
                // RayQuery is a capability/type, not a reflected resource list in SPIRV-Cross.
                // Its capability was already rejected above; querying it returns INVALID_ARGUMENT.
                if (kind == SPVC_RESOURCE_TYPE_RAY_QUERY) continue;
                const spvc_reflected_resource* list{};
                if (!check(spvc_resources_get_resource_list_for_type(resources,
                    static_cast<spvc_resource_type>(kind), &list, &count))) return false;
                if (stage == SpvExecutionModelFragment && kind == SPVC_RESOURCE_TYPE_PUSH_CONSTANT)
                {
                    if (count != 1) return false;
                    const auto structure = spvc_compiler_get_type_handle(compiler, list[0].base_type_id);
                    if (spvc_type_get_num_member_types(structure) != 1) return false;
                    const auto member = spvc_compiler_get_type_handle(compiler,
                        spvc_type_get_member_type(structure, 0));
                    std::size_t size = 0;
                    if (!check(spvc_compiler_get_declared_struct_size(compiler, structure, &size))) return false;
                    const bool is_abi_mismatch = size != 16 || !float4(member) ||
                        spvc_compiler_get_member_decoration(compiler, list[0].base_type_id, 0, SpvDecorationOffset) != 0;
                    if (is_abi_mismatch) return false;
                }
                else if (abi == 2 && stage == SpvExecutionModelFragment && kind == SPVC_RESOURCE_TYPE_SAMPLED_IMAGE)
                {
                    if (count != 1) return false;
                    const auto type = spvc_compiler_get_type_handle(compiler, list[0].type_id);
                    const bool invalid_array = spvc_type_get_num_array_dimensions(type) != 1 ||
                        spvc_type_get_array_dimension(type, 0) != 0;
                    const bool invalid_image = spvc_type_get_image_dimension(type) != SpvDim2D ||
                        spvc_type_get_image_arrayed(type) || spvc_type_get_image_is_depth(type) ||
                        spvc_type_get_image_multisampled(type) || spvc_type_get_image_is_storage(type);
                    const auto sampled_type = spvc_compiler_get_type_handle(compiler, spvc_type_get_image_sampled_type(type));
                    const bool invalid_binding = spvc_compiler_get_decoration(compiler, list[0].id, SpvDecorationDescriptorSet) != 0 ||
                        spvc_compiler_get_decoration(compiler, list[0].id, SpvDecorationBinding) != 0;
                    if (invalid_array || invalid_image || invalid_binding ||
                        spvc_type_get_basetype(sampled_type) != SPVC_BASETYPE_FP32) return false;
                }
                else if (stage == SpvExecutionModelFragment && kind == SPVC_RESOURCE_TYPE_STAGE_OUTPUT)
                {
                    if (count != 1) return false;
                    const bool is_output_mismatch = !float4(spvc_compiler_get_type_handle(compiler, list[0].type_id)) ||
                        spvc_compiler_get_decoration(compiler, list[0].id, SpvDecorationLocation) != 0;
                    if (is_output_mismatch) return false;
                }
                else if (count != 0) return false;
            }
            return true;
        }
    }

    RenderResult<VkPipeline> compileGraphics(
        const VulkanDevice& device, VkPipelineLayout layout, const ShaderArtifact& shader
    ) noexcept
    {
        const bool is_unsupported = (shader.abi != 1 && shader.abi != 2) || !device.dynamicRendering() ||
            (shader.abi == 2 && !device.textureDescriptors());
        if (is_unsupported) return cxx::unexpected(RenderError{kUnsupported});
        if (!validateStage(shader.vertex, SpvExecutionModelVertex, shader.abi) ||
            !validateStage(shader.fragment, SpvExecutionModelFragment, shader.abi))
            return cxx::unexpected(RenderError{kInvalidArgument});
        struct Modules final
        {
            VkDevice device;
            std::array<VkShaderModule, 2> handles{};
            ~Modules() noexcept
            {
                for (auto shader : handles)
                    if (shader) LUX_DESTROY("shader", vkDestroyShaderModule(device, shader, nullptr));
            }
        } modules{device.native()};
        const std::array code{std::span(shader.vertex), std::span(shader.fragment)};
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        for (std::size_t i = 0; i < 2; ++i)
        {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = code[i].size_bytes();
            info.pCode = code[i].data();
            const auto created = LUX_NATIVE("shader", vkCreateShaderModule(device.native(), &info, nullptr,
                &modules.handles[i]));
            if (created != VK_SUCCESS) return cxx::unexpected(nativeError(created));
            stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[i].module = modules.handles[i];
            stages[i].pName = "main";
        }
        // Same dynamic-rendering recipe as the audited R4 Graphics.cpp subset: no vertex buffers,
        // one RGBA8 attachment, no depth/blend, dynamic viewport/scissor, triangle list.
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        input.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        const VkDynamicState dynamic_states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages.data();
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &input;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &samples;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        VkPipeline pipeline{};
        const auto result = LUX_NATIVE("pipeline", vkCreateGraphicsPipelines(device.native(), {}, 1, &info, nullptr,
            &pipeline));
        if (result != VK_SUCCESS)
        {
            if (pipeline) LUX_DESTROY("pipeline", vkDestroyPipeline(device.native(), pipeline, nullptr));
            return cxx::unexpected(nativeError(result));
        }
        return pipeline;
    }
}
