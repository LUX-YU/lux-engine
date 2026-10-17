#include "Shader.hpp"
#include <lux/engine/render/resource/Storage.hpp>
#include "Native.hpp"

#include <spirv-tools/libspirv.h>
#include <spirv_cross/spirv_cross_c.h>
#include <algorithm>
#include <bit>
#include <lux/engine/gapi/vk/Descriptor.hpp>
#include <exception>
#include <memory>

namespace lux::render::vulkan
{
    VertexFormat vertexFormat(rdesc::ETextureFormat format) noexcept
    {
        using F = rdesc::ETextureFormat;
        using S = rdesc::EDataScalar;
        switch (format)
        {
        case F::R32_SFLOAT: return {VK_FORMAT_R32_SFLOAT, S::FLOAT, 1};
        case F::RG32_SFLOAT: return {VK_FORMAT_R32G32_SFLOAT, S::FLOAT, 2};
        case F::RGB32_SFLOAT: return {VK_FORMAT_R32G32B32_SFLOAT, S::FLOAT, 3};
        case F::RGBA32_SFLOAT: return {VK_FORMAT_R32G32B32A32_SFLOAT, S::FLOAT, 4};
        case F::R32_UINT: return {VK_FORMAT_R32_UINT, S::UINT, 1};
        case F::RG32_UINT: return {VK_FORMAT_R32G32_UINT, S::UINT, 2};
        case F::RGB32_UINT: return {VK_FORMAT_R32G32B32_UINT, S::UINT, 3};
        case F::RGBA32_UINT: return {VK_FORMAT_R32G32B32A32_UINT, S::UINT, 4};
        case F::R32_SINT: return {VK_FORMAT_R32_SINT, S::SINT, 1};
        case F::RG32_SINT: return {VK_FORMAT_R32G32_SINT, S::SINT, 2};
        case F::RGB32_SINT: return {VK_FORMAT_R32G32B32_SINT, S::SINT, 3};
        case F::RGBA32_SINT: return {VK_FORMAT_R32G32B32A32_SINT, S::SINT, 4};
        default: return {};
        }
    }

    namespace
    {
        struct StageField final
        {
            unsigned location, components;
            spvc_basetype scalar;
            bool flat;
            bool operator==(const StageField&) const noexcept = default;
        };
        struct StageIo final { std::vector<StageField> inputs, outputs; };
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

        // Adapted from existing DataValidation.cpp: walk SPIRV-Cross types, not SPIR-V instructions.
        bool validateFields(
            spvc_compiler compiler, spvc_type type, std::string path, std::uint64_t base,
            std::uint32_t inherited_stride, const ShaderBlock& expected, std::size_t& cursor
        ) noexcept
        {
            const auto structure_id = spvc_type_get_base_type_id(type);
            for (unsigned member_index = 0; member_index < spvc_type_get_num_member_types(type); ++member_index)
            {
                const auto member = spvc_compiler_get_type_handle(
                    compiler, spvc_type_get_member_type(type, member_index)
                );
                unsigned member_offset = 0;
                const auto offset_result = spvc_compiler_type_struct_member_offset(
                    compiler, type, member_index, &member_offset
                );
                if (!check(offset_result)) return false;
                const auto offset = base + member_offset;
                if (offset > expected.size) return false;
                const auto name = path + spvc_compiler_get_member_name(compiler, structure_id, member_index);
                const auto dimensions = spvc_type_get_num_array_dimensions(member);
                unsigned count = 1, stride = inherited_stride;
                if (dimensions)
                {
                    if (dimensions != 1 || !spvc_type_array_dimension_is_literal(member, 0)) return false;
                    count = spvc_type_get_array_dimension(member, 0);
                    if (!count || count > expected.fields.size()) return false;
                    if (!check(spvc_compiler_type_struct_member_array_stride(compiler, type, member_index, &stride)))
                        return false;
                }
                for (unsigned item = 0; item < count; ++item)
                {
                    const auto at = offset + (dimensions ? std::uint64_t(item) * stride : 0);
                    if (at > expected.size) return false;
                    const auto item_name = name + (dimensions ? "[" + std::to_string(item) + "]" : "");
                    const auto scalar = spvc_type_get_basetype(member);
                    if (scalar == SPVC_BASETYPE_STRUCT)
                    {
                        const bool valid = validateFields(
                            compiler, member, item_name + ".", at, stride, expected, cursor
                        );
                        if (!valid) return false;
                        continue;
                    }
                    const bool numeric = scalar == SPVC_BASETYPE_FP32 || scalar == SPVC_BASETYPE_INT32 ||
                        scalar == SPVC_BASETYPE_UINT32;
                    const bool row_major = spvc_compiler_has_member_decoration(
                        compiler, structure_id, member_index, SpvDecorationRowMajor
                    );
                    const bool invalid = !numeric || spvc_type_get_bit_width(member) != 32 || row_major ||
                        cursor >= expected.fields.size();
                    if (invalid) return false;
                    unsigned matrix_stride = 0;
                    if (spvc_type_get_columns(member) > 1)
                    {
                        const auto result = spvc_compiler_type_struct_member_matrix_stride(
                            compiler, type, member_index, &matrix_stride
                        );
                        if (!check(result)) return false;
                    }
                    const auto scalar_kind = scalar == SPVC_BASETYPE_FP32 ? rdesc::EDataScalar::FLOAT :
                        scalar == SPVC_BASETYPE_INT32 ? rdesc::EDataScalar::SINT : rdesc::EDataScalar::UINT;
                    const auto& field = expected.fields[cursor++];
                    const bool mismatch = field.path != item_name || field.scalar != scalar_kind ||
                        field.offset != at || field.rows != spvc_type_get_vector_size(member) ||
                        field.columns != spvc_type_get_columns(member) ||
                        field.array_stride != stride || field.matrix_stride != matrix_stride;
                    if (mismatch) return false;
                }
            }
            return true;
        }

        bool validateBlock(spvc_compiler compiler, spvc_type block, const ShaderBlock& expected) noexcept
        {
            const bool invalid = !expected.size || !std::has_single_bit(expected.alignment) ||
                expected.fields.empty() ||
                (expected.storage != rdesc::EDataLayout::STD140 && expected.storage != rdesc::EDataLayout::STD430);
            if (invalid) return false;
            auto record = block;
            if (expected.runtime_records)
            {
                if (spvc_type_get_num_member_types(block) != 1) return false;
                unsigned offset = 0, stride = 0;
                if (!check(spvc_compiler_type_struct_member_offset(compiler, block, 0, &offset)) || offset != 0 ||
                    !check(spvc_compiler_type_struct_member_array_stride(compiler, block, 0, &stride)) ||
                    stride != expected.size) return false;
                record = spvc_compiler_get_type_handle(compiler, spvc_type_get_member_type(block, 0));
                const bool wrong_record = spvc_type_get_basetype(record) != SPVC_BASETYPE_STRUCT ||
                    spvc_type_get_num_array_dimensions(record) != 1 || spvc_type_get_array_dimension(record, 0) != 0;
                if (wrong_record) return false;
            }
            std::size_t size = 0, cursor = 0;
            if (!check(spvc_compiler_get_declared_struct_size(compiler, record, &size))) return false;
            const auto rounded = (std::uint64_t(size) + expected.alignment - 1) /
                expected.alignment * expected.alignment;
            return rounded == expected.size && validateFields(compiler, record, "", 0, 0, expected, cursor) &&
                cursor == expected.fields.size();
        }

        bool validateStage(
            const SpirvStage& input, SpvExecutionModel stage, const ShaderInterface& interface,
            const VkPhysicalDeviceLimits& limits, const GraphicsState* graphics = nullptr, StageIo* io = nullptr
        ) noexcept
        {
            const bool invalid_entry = input.entry.empty() || input.entry.find('\0') != std::string::npos;
            if (invalid_entry) return false;
            const auto stage_bit = stage == SpvExecutionModelVertex ? 1u : stage == SpvExecutionModelFragment ? 2u : 4u;
            const auto words = std::span(input.words);
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
                const bool sampled = stage == SpvExecutionModelFragment &&
                    std::ranges::any_of(interface.bindings, [](const auto& value) {
                        return value.kind == EDescriptorKind::SAMPLED_IMAGE_ARRAY && value.count == 0;
                    }) &&
                    (capability == SpvCapabilityRuntimeDescriptorArray || capability == SpvCapabilityShaderNonUniform ||
                     capability == SpvCapabilitySampledImageArrayNonUniformIndexing ||
                     capability == SpvCapabilitySampledImageArrayDynamicIndexing || capability == SpvCapabilityImageQuery);
                if (capability != SpvCapabilityShader && !sampled) return false;
            }
            const spvc_entry_point* entries{};
            std::size_t count = 0;
            if (!check(spvc_compiler_get_entry_points(compiler, &entries, &count))) return false;
            const bool is_entry_mismatch = count != 1 || entries[0].execution_model != stage ||
                input.entry != entries[0].name;
            if (is_entry_mismatch) return false;
            if (stage == SpvExecutionModelGLCompute)
            {
                const SpvExecutionMode* modes{};
                std::size_t mode_count = 0;
                if (!check(spvc_compiler_get_execution_modes(compiler, &modes, &mode_count))) return false;
                const auto mode_list = std::span(modes, mode_count);
                const bool has_local = std::ranges::find(mode_list, SpvExecutionModeLocalSize) != mode_list.end();
                const bool specialized = std::ranges::find(mode_list, SpvExecutionModeLocalSizeId) != mode_list.end();
                if (!has_local || specialized) return false;
                std::uint64_t invocations = 1;
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    const auto size = spvc_compiler_get_execution_mode_argument_by_index(
                        compiler, SpvExecutionModeLocalSize, axis);
                    if (!size || size > limits.maxComputeWorkGroupSize[axis]) return false;
                    invocations *= size;
                    if (invocations > limits.maxComputeWorkGroupInvocations) return false;
                }
            }
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
                if (kind == SPVC_RESOURCE_TYPE_PUSH_CONSTANT)
                {
                    const bool declared = (interface.parameter_stages & stage_bit) != 0;
                    if (count != unsigned(declared)) return false;
                    if (declared)
                    {
                        const auto structure = spvc_compiler_get_type_handle(compiler, list[0].base_type_id);
                        if (!validateBlock(compiler, structure, interface.parameters)) return false;
                    }
                }
                else if (kind == SPVC_RESOURCE_TYPE_SAMPLED_IMAGE || kind == SPVC_RESOURCE_TYPE_STORAGE_BUFFER)
                {
                    const auto expected_kind = kind == SPVC_RESOURCE_TYPE_SAMPLED_IMAGE ?
                        EDescriptorKind::SAMPLED_IMAGE_ARRAY : EDescriptorKind::STORAGE_BUFFER;
                    const auto expected_count = std::ranges::count_if(interface.bindings,
                        [&](const auto& value) { return value.kind == expected_kind && (value.stages & stage_bit); });
                    if (count != expected_count) return false;
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const auto set = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationDescriptorSet);
                        const auto binding = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
                        const auto expected = std::ranges::find_if(interface.bindings, [&](const auto& value) {
                            return value.set == set && value.binding == binding && value.kind == expected_kind &&
                                (value.stages & stage_bit) && value.name == spvc_compiler_get_name(compiler, list[i].id);
                        });
                        if (expected == interface.bindings.end()) return false;
                        const auto type = spvc_compiler_get_type_handle(compiler, list[i].type_id);
                        if (expected_kind == EDescriptorKind::SAMPLED_IMAGE_ARRAY)
                        {
                            const bool invalid_array = spvc_type_get_num_array_dimensions(type) != 1 ||
                                spvc_type_get_array_dimension(type, 0) != expected->count;
                            const bool invalid_image = spvc_type_get_image_dimension(type) != SpvDim2D ||
                                spvc_type_get_image_arrayed(type) || spvc_type_get_image_is_depth(type) ||
                                spvc_type_get_image_multisampled(type) || spvc_type_get_image_is_storage(type);
                            const auto sampled = spvc_compiler_get_type_handle(compiler,
                                spvc_type_get_image_sampled_type(type));
                            if (invalid_array || invalid_image ||
                                spvc_type_get_basetype(sampled) != SPVC_BASETYPE_FP32) return false;
                        }
                        else
                        {
                            const SpvDecoration* decorations{};
                            std::size_t decoration_count = 0;
                            if (!check(spvc_compiler_get_buffer_block_decorations(
                                compiler, list[i].id, &decorations, &decoration_count))) return false;
                            const auto flags = std::span(decorations, decoration_count);
                            const bool read = std::ranges::find(flags, SpvDecorationNonReadable) == flags.end();
                            const bool write = std::ranges::find(flags, SpvDecorationNonWritable) == flags.end();
                            const bool access_mismatch = read != (expected->access != EStorageAccess::WRITE_ONLY) ||
                                write != (expected->access != EStorageAccess::READ_ONLY);
                            if (access_mismatch) return false;
                            const auto structure = spvc_compiler_get_type_handle(compiler, list[i].base_type_id);
                            const bool invalid_block = spvc_type_get_num_array_dimensions(type) != 0 ||
                                expected->count != 1 || !validateBlock(compiler, structure, expected->block);
                            if (invalid_block) return false;
                        }
                    }
                }
                else if (stage == SpvExecutionModelVertex && kind == SPVC_RESOURCE_TYPE_STAGE_INPUT)
                {
                    if (!graphics || count != graphics->vertex_attributes.size()) return false;
                    for (std::size_t item = 0; item < count; ++item)
                    {
                        const auto location = spvc_compiler_get_decoration(
                            compiler, list[item].id, SpvDecorationLocation
                        );
                        const auto found = std::ranges::find(
                            graphics->vertex_attributes, location, &VertexAttribute::location
                        );
                        if (found == graphics->vertex_attributes.end()) return false;
                        const auto format = vertexFormat(found->format);
                        const auto type = spvc_compiler_get_type_handle(compiler, list[item].type_id);
                        const auto scalar = format.scalar == rdesc::EDataScalar::FLOAT ? SPVC_BASETYPE_FP32 :
                            format.scalar == rdesc::EDataScalar::UINT ? SPVC_BASETYPE_UINT32 : SPVC_BASETYPE_INT32;
                        const bool mismatch = spvc_type_get_basetype(type) != scalar ||
                            spvc_type_get_vector_size(type) != format.components || spvc_type_get_columns(type) != 1 ||
                            spvc_type_get_num_array_dimensions(type) != 0;
                        if (mismatch) return false;
                    }
                }
                else if ((stage == SpvExecutionModelVertex && kind == SPVC_RESOURCE_TYPE_STAGE_OUTPUT) ||
                    (stage == SpvExecutionModelFragment && kind == SPVC_RESOURCE_TYPE_STAGE_INPUT))
                {
                    if (!io) return false;
                    auto& fields = kind == SPVC_RESOURCE_TYPE_STAGE_INPUT ? io->inputs : io->outputs;
                    for (std::size_t item = 0; item < count; ++item)
                    {
                        const auto type = spvc_compiler_get_type_handle(compiler, list[item].type_id);
                        const auto scalar = spvc_type_get_basetype(type);
                        const bool numeric = scalar == SPVC_BASETYPE_FP32 || scalar == SPVC_BASETYPE_UINT32 ||
                            scalar == SPVC_BASETYPE_INT32;
                        const bool invalid = !numeric || spvc_type_get_columns(type) != 1 ||
                            spvc_type_get_num_array_dimensions(type) != 0;
                        if (invalid) return false;
                        fields.push_back({spvc_compiler_get_decoration(compiler, list[item].id, SpvDecorationLocation),
                            spvc_type_get_vector_size(type), scalar,
                            bool(spvc_compiler_has_decoration(compiler, list[item].id, SpvDecorationFlat))});
                    }
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

    RenderResult<VkDescriptorSetLayout> createDescriptorLayout(
        const VulkanDevice& device, std::span<const ShaderBinding> contract, std::uint32_t runtime_count,
        const char* fault_label
    ) noexcept
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorBindingFlags> flags;
        bool update_after_bind = false;
        for (const auto& binding : contract)
        {
            const bool runtime_array = binding.count == 0;
            const auto count = runtime_array ? runtime_count : binding.count;
            if (!count) return cxx::unexpected(RenderError{kStorageContract});
            const auto type = binding.kind == EDescriptorKind::SAMPLED_IMAGE_ARRAY ?
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            const VkShaderStageFlags stages = ((binding.stages & 1u) ? VK_SHADER_STAGE_VERTEX_BIT : 0u) |
                ((binding.stages & 2u) ? VK_SHADER_STAGE_FRAGMENT_BIT : 0u) |
                ((binding.stages & 4u) ? VK_SHADER_STAGE_COMPUTE_BIT : 0u);
            bindings.push_back({binding.binding, type, count, stages, nullptr});
            flags.push_back(runtime_array ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT : 0);
            update_after_bind |= runtime_array;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, nullptr,
            std::uint32_t(flags.size()), flags.data()};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.pNext = &binding_flags;
        layout.flags = update_after_bind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
        layout.bindingCount = std::uint32_t(bindings.size());
        layout.pBindings = bindings.data();
        gapi::vk::DescriptorSetLayout handle;
        static_assert(sizeof(handle) == sizeof(VkDescriptorSetLayout));
        const auto result = LUX_NATIVE(fault_label, handle.tryCreate(device.native(), layout));
        if (result != VK_SUCCESS) return cxx::unexpected(nativeError(result));
        return handle.handle(); // Transfer the handle to the caller's existing aggregate owner.
    }

    RenderResult<VkPipeline> compileCompute(
        const VulkanDevice& device, VkPipelineLayout layout, const SpirvArtifact& shader
    ) noexcept
    {
        const bool wrong_stage = shader.stages.size() != 1 ||
            shader.stages[0].stage != rdesc::EShaderType::COMPUTE;
        if (wrong_stage) return cxx::unexpected(RenderError{kInvalidArgument});
        const auto& code = shader.stages[0];
        if (!validateStage(code, SpvExecutionModelGLCompute, shader.interface, device.properties().limits))
            return cxx::unexpected(RenderError{kInvalidArgument});
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = code.words.size() * sizeof(std::uint32_t);
        module.pCode = code.words.data();
        VkShaderModule handle{};
        const auto created = LUX_NATIVE("shader", vkCreateShaderModule(device.native(), &module, nullptr, &handle));
        if (created != VK_SUCCESS) return cxx::unexpected(nativeError(created));
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = handle;
        info.stage.pName = code.entry.c_str();
        info.layout = layout;
        VkPipeline pipeline{};
        const auto result = LUX_NATIVE("pipeline", vkCreateComputePipelines(
            device.native(), {}, 1, &info, nullptr, &pipeline));
        LUX_DESTROY("shader", vkDestroyShaderModule(device.native(), handle, nullptr));
        if (result != VK_SUCCESS)
        {
            if (pipeline) LUX_DESTROY("pipeline", vkDestroyPipeline(device.native(), pipeline, nullptr));
            return cxx::unexpected(nativeError(result));
        }
        return pipeline;
    }

    RenderResult<VkPipeline> compileGraphics(
        const VulkanDevice& device, VkPipelineLayout layout, const SpirvArtifact& shader, const GraphicsState& graphics
    ) noexcept
    {
        const bool needs_arrays = std::ranges::any_of(shader.interface.bindings, [](const auto& value) {
            return value.kind == EDescriptorKind::SAMPLED_IMAGE_ARRAY && value.count == 0;
        });
        const bool is_unsupported = !device.dynamicRendering() ||
            (needs_arrays && !device.textureDescriptors());
        if (is_unsupported) return cxx::unexpected(RenderError{kUnsupported});
        if (shader.stages.size() != 2) return cxx::unexpected(RenderError{kInvalidArgument});
        const SpirvStage* vertex_stage = nullptr;
        const SpirvStage* fragment_stage = nullptr;
        for (const auto& stage : shader.stages)
        {
            if (stage.stage == rdesc::EShaderType::VERTEX) vertex_stage = &stage;
            else if (stage.stage == rdesc::EShaderType::FRAGMENT) fragment_stage = &stage;
        }
        const bool missing_stage = !vertex_stage || !fragment_stage;
        if (missing_stage) return cxx::unexpected(RenderError{kInvalidArgument});
        const auto& limits = device.properties().limits;
        StageIo vertex_io, fragment_io;
        const bool valid_vertex = validateStage(*vertex_stage, SpvExecutionModelVertex, shader.interface,
            limits, &graphics, &vertex_io);
        const bool valid_fragment = validateStage(*fragment_stage, SpvExecutionModelFragment, shader.interface,
            limits, &graphics, &fragment_io);
        if (!valid_vertex || !valid_fragment) return cxx::unexpected(RenderError{kInvalidArgument});
        for (const auto& input : fragment_io.inputs)
            if (std::ranges::find(vertex_io.outputs, input) == vertex_io.outputs.end())
                return cxx::unexpected(RenderError{kInvalidArgument});
        if (graphics.vertex_bindings.size() > limits.maxVertexInputBindings ||
            graphics.vertex_attributes.size() > limits.maxVertexInputAttributes)
            return cxx::unexpected(RenderError{kUnsupported});
        std::vector<VkVertexInputBindingDescription> vertex_bindings;
        std::vector<VkVertexInputAttributeDescription> vertex_attributes;
        for (const auto& binding : graphics.vertex_bindings)
        {
            const bool invalid = !binding.stride || binding.stride > limits.maxVertexInputBindingStride ||
                binding.binding >= limits.maxVertexInputBindings || binding.rate > EVertexRate::INSTANCE ||
                std::ranges::any_of(vertex_bindings, [&](const auto& other) {
                    return other.binding == binding.binding;
                });
            if (invalid) return cxx::unexpected(RenderError{kInvalidArgument});
            vertex_bindings.push_back({binding.binding, binding.stride,
                binding.rate == EVertexRate::VERTEX ? VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE});
        }
        for (const auto& field : graphics.vertex_attributes)
        {
            const auto format = vertexFormat(field.format);
            const auto binding = std::ranges::find(
                vertex_bindings, field.binding, &VkVertexInputBindingDescription::binding
            );
            const bool invalid = !format.components || field.location >= limits.maxVertexInputAttributes ||
                field.offset > limits.maxVertexInputAttributeOffset || binding == vertex_bindings.end() ||
                std::ranges::any_of(vertex_attributes, [&](const auto& other) {
                    return other.location == field.location;
                });
            if (invalid) return cxx::unexpected(RenderError{kInvalidArgument});
            if (std::uint64_t(field.offset) + format.bytes() > binding->stride)
                return cxx::unexpected(RenderError{kInvalidArgument});
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(device.physical(), format.native, &properties);
            if (!(properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
                return cxx::unexpected(RenderError{kUnsupported});
            vertex_attributes.push_back({field.location, field.binding, format.native, field.offset});
        }
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
        const std::array code{vertex_stage, fragment_stage};
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        for (std::size_t i = 0; i < 2; ++i)
        {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = code[i]->words.size() * sizeof(std::uint32_t);
            info.pCode = code[i]->words.data();
            const auto created = LUX_NATIVE("shader", vkCreateShaderModule(device.native(), &info, nullptr,
                &modules.handles[i]));
            if (created != VK_SUCCESS) return cxx::unexpected(nativeError(created));
            stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[i].module = modules.handles[i];
            stages[i].pName = code[i]->entry.c_str();
        }
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertex.vertexBindingDescriptionCount = std::uint32_t(vertex_bindings.size());
        vertex.pVertexBindingDescriptions = vertex_bindings.data();
        vertex.vertexAttributeDescriptionCount = std::uint32_t(vertex_attributes.size());
        vertex.pVertexAttributeDescriptions = vertex_attributes.data();
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
