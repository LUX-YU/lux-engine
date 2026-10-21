#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/Shader.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <memory>
#include <spirv-tools/libspirv.h>
#include <spirv_cross/spirv_cross_c.h>

namespace lux::render::detail
{
    VertexFormat vertexFormat(rdesc::ETextureFormat format) noexcept
    {
        using F = rdesc::ETextureFormat;
        using S = rdesc::EDataScalar;
        switch (format)
        {
        case F::R32_SFLOAT:
            return {nri::Format::R32_SFLOAT, S::FLOAT, 1};
        case F::RG32_SFLOAT:
            return {nri::Format::RG32_SFLOAT, S::FLOAT, 2};
        case F::RGB32_SFLOAT:
            return {nri::Format::RGB32_SFLOAT, S::FLOAT, 3};
        case F::RGBA32_SFLOAT:
            return {nri::Format::RGBA32_SFLOAT, S::FLOAT, 4};
        case F::R32_UINT:
            return {nri::Format::R32_UINT, S::UINT, 1};
        case F::RG32_UINT:
            return {nri::Format::RG32_UINT, S::UINT, 2};
        case F::RGB32_UINT:
            return {nri::Format::RGB32_UINT, S::UINT, 3};
        case F::RGBA32_UINT:
            return {nri::Format::RGBA32_UINT, S::UINT, 4};
        case F::R32_SINT:
            return {nri::Format::R32_SINT, S::SINT, 1};
        case F::RG32_SINT:
            return {nri::Format::RG32_SINT, S::SINT, 2};
        case F::RGB32_SINT:
            return {nri::Format::RGB32_SINT, S::SINT, 3};
        case F::RGBA32_SINT:
            return {nri::Format::RGBA32_SINT, S::SINT, 4};
        default:
            return {};
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

        struct StageIo final
        {
            std::vector<StageField> inputs, outputs;
        };

        bool check(spvc_result result) noexcept
        {
            if (result == SPVC_ERROR_OUT_OF_MEMORY)
            {
                std::terminate();
            }
            return result == SPVC_SUCCESS;
        }

        bool float4(spvc_type type) noexcept
        {
            return spvc_type_get_basetype(type) == SPVC_BASETYPE_FP32 && spvc_type_get_vector_size(type) == 4 &&
                   spvc_type_get_columns(type) == 1 && spvc_type_get_num_array_dimensions(type) == 0;
        }

        // Adapted from existing DataValidation.cpp: walk SPIRV-Cross types, not SPIR-V instructions.
        bool validateFields(
            spvc_compiler compiler,
            spvc_type type,
            std::string path,
            std::uint64_t base,
            std::uint32_t inherited_stride,
            const rdesc::ShaderBlock& expected,
            std::size_t& cursor
        ) noexcept
        {
            const auto structure_id = spvc_type_get_base_type_id(type);
            for (unsigned member_index = 0; member_index < spvc_type_get_num_member_types(type); ++member_index)
            {
                const auto member =
                    spvc_compiler_get_type_handle(compiler, spvc_type_get_member_type(type, member_index));
                unsigned member_offset = 0;
                const auto offset_result =
                    spvc_compiler_type_struct_member_offset(compiler, type, member_index, &member_offset);
                if (!check(offset_result))
                {
                    return false;
                }
                const auto offset = base + member_offset;
                if (offset > expected.size)
                {
                    return false;
                }
                const auto name = path + spvc_compiler_get_member_name(compiler, structure_id, member_index);
                const auto dimensions = spvc_type_get_num_array_dimensions(member);
                unsigned count = 1, stride = inherited_stride;
                if (dimensions)
                {
                    if (dimensions != 1 || !spvc_type_array_dimension_is_literal(member, 0))
                    {
                        return false;
                    }
                    count = spvc_type_get_array_dimension(member, 0);
                    if (!count || count > expected.fields.size())
                    {
                        return false;
                    }
                    if (!check(spvc_compiler_type_struct_member_array_stride(compiler, type, member_index, &stride)))
                    {
                        return false;
                    }
                }
                for (unsigned item = 0; item < count; ++item)
                {
                    const auto at = offset + (dimensions ? std::uint64_t(item) * stride : 0);
                    if (at > expected.size)
                    {
                        return false;
                    }
                    const auto item_name = name + (dimensions ? "[" + std::to_string(item) + "]" : "");
                    const auto scalar = spvc_type_get_basetype(member);
                    if (scalar == SPVC_BASETYPE_STRUCT)
                    {
                        const bool valid =
                            validateFields(compiler, member, item_name + ".", at, stride, expected, cursor);
                        if (!valid)
                        {
                            return false;
                        }
                        continue;
                    }
                    const bool numeric =
                        scalar == SPVC_BASETYPE_FP32 || scalar == SPVC_BASETYPE_INT32 || scalar == SPVC_BASETYPE_UINT32;
                    const bool row_major = spvc_compiler_has_member_decoration(
                        compiler,
                        structure_id,
                        member_index,
                        SpvDecorationRowMajor
                    );
                    const bool invalid = !numeric || spvc_type_get_bit_width(member) != 32 || row_major ||
                                         cursor >= expected.fields.size();
                    if (invalid)
                    {
                        return false;
                    }
                    unsigned matrix_stride = 0;
                    if (spvc_type_get_columns(member) > 1)
                    {
                        const auto result = spvc_compiler_type_struct_member_matrix_stride(
                            compiler,
                            type,
                            member_index,
                            &matrix_stride
                        );
                        if (!check(result))
                        {
                            return false;
                        }
                    }
                    const auto scalar_kind = scalar == SPVC_BASETYPE_FP32    ? rdesc::EDataScalar::FLOAT
                                             : scalar == SPVC_BASETYPE_INT32 ? rdesc::EDataScalar::SINT
                                                                             : rdesc::EDataScalar::UINT;
                    const auto& field = expected.fields[cursor++];
                    const bool mismatch = field.path != item_name || field.scalar != scalar_kind ||
                                          field.offset != at || field.rows != spvc_type_get_vector_size(member) ||
                                          field.columns != spvc_type_get_columns(member) ||
                                          field.array_stride != stride || field.matrix_stride != matrix_stride;
                    if (mismatch)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        bool validateBlock(spvc_compiler compiler, spvc_type block, const rdesc::ShaderBlock& expected) noexcept
        {
            const bool invalid =
                !expected.size || !std::has_single_bit(expected.alignment) || expected.fields.empty() ||
                (expected.storage != rdesc::EDataLayout::STD140 && expected.storage != rdesc::EDataLayout::STD430);
            if (invalid)
            {
                return false;
            }
            auto record = block;
            if (expected.runtime_records)
            {
                if (spvc_type_get_num_member_types(block) != 1)
                {
                    return false;
                }
                unsigned offset = 0, stride = 0;
                if (!check(spvc_compiler_type_struct_member_offset(compiler, block, 0, &offset)) || offset != 0 ||
                    !check(spvc_compiler_type_struct_member_array_stride(compiler, block, 0, &stride)) ||
                    stride != expected.size)
                {
                    return false;
                }
                record = spvc_compiler_get_type_handle(compiler, spvc_type_get_member_type(block, 0));
                const bool wrong_record =
                    spvc_type_get_num_array_dimensions(record) != 1 || spvc_type_get_array_dimension(record, 0) != 0;
                if (wrong_record)
                {
                    return false;
                }
                if (spvc_type_get_basetype(record) != SPVC_BASETYPE_STRUCT)
                {
                    const auto scalar = spvc_type_get_basetype(record);
                    const bool numeric =
                        scalar == SPVC_BASETYPE_FP32 || scalar == SPVC_BASETYPE_INT32 || scalar == SPVC_BASETYPE_UINT32;
                    if (!numeric || spvc_type_get_bit_width(record) != 32 || expected.fields.size() != 1)
                    {
                        return false;
                    }
                    const auto structure = spvc_type_get_base_type_id(block);
                    if (spvc_compiler_has_member_decoration(compiler, structure, 0, SpvDecorationRowMajor))
                    {
                        return false;
                    }
                    unsigned matrix_stride = 0;
                    if (spvc_type_get_columns(record) > 1 &&
                        !check(spvc_compiler_type_struct_member_matrix_stride(compiler, block, 0, &matrix_stride)))
                    {
                        return false;
                    }
                    const auto kind = scalar == SPVC_BASETYPE_FP32    ? rdesc::EDataScalar::FLOAT
                                      : scalar == SPVC_BASETYPE_INT32 ? rdesc::EDataScalar::SINT
                                                                      : rdesc::EDataScalar::UINT;
                    const auto& field = expected.fields.front();
                    return field.path == spvc_compiler_get_member_name(compiler, structure, 0) &&
                           field.scalar == kind && field.offset == 0 &&
                           field.rows == spvc_type_get_vector_size(record) &&
                           field.columns == spvc_type_get_columns(record) && field.array_stride == 0 &&
                           field.matrix_stride == matrix_stride;
                }
            }
            std::size_t size = 0, cursor = 0;
            if (!check(spvc_compiler_get_declared_struct_size(compiler, record, &size)))
            {
                return false;
            }
            const auto rounded =
                (std::uint64_t(size) + expected.alignment - 1) / expected.alignment * expected.alignment;
            // ArrayStride was checked against SPIR-V above. A runtime record can have trailing
            // padding that is absent from its declared member extent; do not infer that ABI again.
            const bool size_matches = expected.runtime_records ? size <= expected.size : rounded == expected.size;
            return size_matches && validateFields(compiler, record, "", 0, 0, expected, cursor) &&
                   cursor == expected.fields.size();
        }

        bool validateStage(
            const rdesc::SpirvStage& input,
            SpvExecutionModel stage,
            const ShaderInterface& interface,
            const nri::DeviceDesc& limits,
            const GraphicsState* graphics = nullptr,
            StageIo* io = nullptr,
            std::uint32_t color_count = 1,
            bool device_address = false
        ) noexcept
        {
            const bool invalid_entry = input.entry.empty() || input.entry.find('\0') != std::string::npos;
            if (invalid_entry)
            {
                return false;
            }
            const auto stage_bit = stage == SpvExecutionModelVertex ? 1u : stage == SpvExecutionModelFragment ? 2u : 4u;
            const auto words = std::span(input.words);
            // Use the existing SPIR-V tools, not another shader parser or handwritten instruction decoder.
            const auto validation = spvContextCreate(SPV_ENV_VULKAN_1_3);
            if (!validation)
            {
                std::terminate();
            }
            const auto valid = spvValidateBinary(validation, words.data(), words.size(), nullptr);
            spvContextDestroy(validation);
            if (valid == SPV_ERROR_OUT_OF_MEMORY)
            {
                std::terminate();
            }
            if (valid != SPV_SUCCESS)
            {
                return false;
            }
            spvc_context raw{};
            if (!check(spvc_context_create(&raw)))
            {
                return false;
            }
            const std::unique_ptr<spvc_context_s, decltype(&spvc_context_destroy)> context(raw, spvc_context_destroy);
            spvc_parsed_ir ir{};
            spvc_compiler compiler{};
            spvc_resources resources{};
            if (!check(spvc_context_parse_spirv(raw, words.data(), words.size(), &ir)))
            {
                return false;
            }
            if (!check(spvc_context_create_compiler(
                    raw,
                    SPVC_BACKEND_NONE,
                    ir,
                    SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                    &compiler
                )))
            {
                return false;
            }
            const SpvCapability* capabilities{};
            std::size_t capability_count = 0;
            if (!check(spvc_compiler_get_declared_capabilities(compiler, &capabilities, &capability_count)))
            {
                return false;
            }
            for (std::size_t i = 0; i < capability_count; ++i)
            {
                const auto capability = capabilities[i];
                const bool sampled =
                    stage == SpvExecutionModelFragment &&
                    std::ranges::any_of(
                        interface.bindings,
                        [](const auto& value)
                        { return value.kind == EDescriptorKind::SAMPLED_IMAGE && value.count == 0; }
                    ) &&
                    (capability == SpvCapabilityRuntimeDescriptorArray || capability == SpvCapabilityShaderNonUniform ||
                     capability == SpvCapabilitySampledImageArrayNonUniformIndexing ||
                     capability == SpvCapabilitySampledImageArrayDynamicIndexing ||
                     capability == SpvCapabilityImageQuery);
                const bool physical_addresses =
                    device_address && capability == SpvCapabilityPhysicalStorageBufferAddresses;
                const bool image_query =
                    capability == SpvCapabilityImageQuery &&
                    std::ranges::any_of(
                        interface.bindings,
                        [&](const auto& value)
                        {
                            return (value.stages & stage_bit) && (value.kind == EDescriptorKind::SAMPLED_IMAGE ||
                                                                  value.kind == EDescriptorKind::STORAGE_IMAGE);
                        }
                    );
                if (capability != SpvCapabilityShader && !sampled && !physical_addresses && !image_query)
                {
                    return false;
                }
            }
            const spvc_entry_point* entries{};
            std::size_t count = 0;
            if (!check(spvc_compiler_get_entry_points(compiler, &entries, &count)))
            {
                return false;
            }
            const bool is_entry_mismatch =
                count != 1 || entries[0].execution_model != stage || input.entry != entries[0].name;
            if (is_entry_mismatch)
            {
                return false;
            }
            if (stage == SpvExecutionModelGLCompute)
            {
                const SpvExecutionMode* modes{};
                std::size_t mode_count = 0;
                if (!check(spvc_compiler_get_execution_modes(compiler, &modes, &mode_count)))
                {
                    return false;
                }
                const auto mode_list = std::span(modes, mode_count);
                const bool has_local = std::ranges::find(mode_list, SpvExecutionModeLocalSize) != mode_list.end();
                const bool specialized = std::ranges::find(mode_list, SpvExecutionModeLocalSizeId) != mode_list.end();
                if (!has_local && !specialized)
                {
                    return false;
                }
                spvc_specialization_constant sizes[3]{};
                spvc_compiler_get_work_group_size_specialization_constants(compiler, &sizes[0], &sizes[1], &sizes[2]);
                if (specialized && (sizes[0].id || sizes[1].id || sizes[2].id))
                {
                    return false;
                }
                std::uint64_t invocations = 1;
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    const auto argument = spvc_compiler_get_execution_mode_argument_by_index(
                        compiler,
                        specialized ? SpvExecutionModeLocalSizeId : SpvExecutionModeLocalSize,
                        axis
                    );
                    const auto size =
                        specialized
                            ? spvc_constant_get_scalar_u32(spvc_compiler_get_constant_handle(compiler, argument), 0, 0)
                            : argument;
                    if (!size || size > limits.shaderStage.compute.workGroupMaxDim[axis])
                    {
                        return false;
                    }
                    invocations *= size;
                    if (invocations > limits.shaderStage.compute.workGroupInvocationMaxNum)
                    {
                        return false;
                    }
                }
            }
            if (!check(spvc_compiler_create_shader_resources(compiler, &resources)))
            {
                return false;
            }
            for (unsigned kind = SPVC_RESOURCE_TYPE_UNIFORM_BUFFER; kind <= SPVC_RESOURCE_TYPE_SHADER_RECORD_BUFFER;
                 ++kind)
            {
                // RayQuery is a capability/type, not a reflected resource list in SPIRV-Cross.
                // Its capability was already rejected above; querying it returns INVALID_ARGUMENT.
                if (kind == SPVC_RESOURCE_TYPE_RAY_QUERY)
                {
                    continue;
                }
                const spvc_reflected_resource* list{};
                if (!check(spvc_resources_get_resource_list_for_type(
                        resources,
                        static_cast<spvc_resource_type>(kind),
                        &list,
                        &count
                    )))
                {
                    return false;
                }
                if (kind == SPVC_RESOURCE_TYPE_PUSH_CONSTANT)
                {
                    const bool declared = (interface.parameter_stages & stage_bit) != 0;
                    if (count != unsigned(declared))
                    {
                        return false;
                    }
                    if (declared)
                    {
                        const auto structure = spvc_compiler_get_type_handle(compiler, list[0].base_type_id);
                        if (!validateBlock(compiler, structure, interface.parameters))
                        {
                            return false;
                        }
                    }
                }
                else if (kind == SPVC_RESOURCE_TYPE_SEPARATE_IMAGE || kind == SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS ||
                         kind == SPVC_RESOURCE_TYPE_STORAGE_BUFFER || kind == SPVC_RESOURCE_TYPE_UNIFORM_BUFFER ||
                         kind == SPVC_RESOURCE_TYPE_STORAGE_IMAGE)
                {
                    const auto expected_kind =
                        kind == SPVC_RESOURCE_TYPE_SEPARATE_IMAGE      ? EDescriptorKind::SAMPLED_IMAGE
                        : kind == SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS ? EDescriptorKind::SAMPLER
                        : kind == SPVC_RESOURCE_TYPE_UNIFORM_BUFFER    ? EDescriptorKind::UNIFORM_BUFFER
                        : kind == SPVC_RESOURCE_TYPE_STORAGE_IMAGE     ? EDescriptorKind::STORAGE_IMAGE
                                                                       : EDescriptorKind::STORAGE_BUFFER;
                    const auto expected_count = std::ranges::count_if(
                        interface.bindings,
                        [&](const auto& value) { return value.kind == expected_kind && (value.stages & stage_bit); }
                    );
                    if (count != expected_count)
                    {
                        return false;
                    }
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const auto set = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationDescriptorSet);
                        const auto binding = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
                        const auto expected = std::ranges::find_if(
                            interface.bindings,
                            [&](const auto& value)
                            {
                                return value.set == set && value.binding == binding && value.kind == expected_kind &&
                                       (value.stages & stage_bit) &&
                                       value.name == spvc_compiler_get_name(compiler, list[i].id);
                            }
                        );
                        if (expected == interface.bindings.end())
                        {
                            return false;
                        }
                        const auto type = spvc_compiler_get_type_handle(compiler, list[i].type_id);
                        if (expected_kind == EDescriptorKind::SAMPLED_IMAGE ||
                            expected_kind == EDescriptorKind::STORAGE_IMAGE)
                        {
                            const auto dimensions = spvc_type_get_num_array_dimensions(type);
                            const bool invalid_array =
                                dimensions == 0
                                    ? expected->count != 1
                                    : dimensions != 1 || spvc_type_get_array_dimension(type, 0) != expected->count;
                            const bool invalid_image =
                                spvc_type_get_image_dimension(type) != SpvDim2D ||
                                bool(spvc_type_get_image_arrayed(type)) != expected->image_arrayed ||
                                bool(spvc_type_get_image_is_depth(type)) != expected->image_depth ||
                                spvc_type_get_image_multisampled(type) ||
                                bool(spvc_type_get_image_is_storage(type)) !=
                                    (expected_kind == EDescriptorKind::STORAGE_IMAGE);
                            const auto sampled =
                                spvc_compiler_get_type_handle(compiler, spvc_type_get_image_sampled_type(type));
                            if (invalid_array || invalid_image || spvc_type_get_basetype(sampled) != SPVC_BASETYPE_FP32)
                            {
                                return false;
                            }
                        }
                        else if (expected_kind == EDescriptorKind::SAMPLER)
                        {
                            const auto dimensions = spvc_type_get_num_array_dimensions(type);
                            if (dimensions == 0
                                    ? expected->count != 1
                                    : dimensions != 1 || spvc_type_get_array_dimension(type, 0) != expected->count)
                            {
                                return false;
                            }
                        }
                        else
                        {
                            const SpvDecoration* decorations{};
                            std::size_t decoration_count = 0;
                            if (!check(spvc_compiler_get_buffer_block_decorations(
                                    compiler,
                                    list[i].id,
                                    &decorations,
                                    &decoration_count
                                )))
                            {
                                return false;
                            }
                            const auto flags = std::span(decorations, decoration_count);
                            const bool read = std::ranges::find(flags, SpvDecorationNonReadable) == flags.end();
                            const bool write = std::ranges::find(flags, SpvDecorationNonWritable) == flags.end();
                            const bool access_mismatch = read != (expected->access != EShaderAccess::WRITE_ONLY) ||
                                                         write != (expected->access != EShaderAccess::READ_ONLY);
                            if (expected_kind == EDescriptorKind::STORAGE_BUFFER && access_mismatch)
                            {
                                return false;
                            }
                            const auto structure = spvc_compiler_get_type_handle(compiler, list[i].base_type_id);
                            const auto dimensions = spvc_type_get_num_array_dimensions(type);
                            const bool wrong_array =
                                dimensions == 0
                                    ? expected->count != 1
                                    : dimensions != 1 || spvc_type_get_array_dimension(type, 0) != expected->count;
                            const bool invalid_block =
                                wrong_array || !validateBlock(compiler, structure, expected->block);
                            if (invalid_block)
                            {
                                return false;
                            }
                        }
                    }
                }
                else if (stage == SpvExecutionModelVertex && kind == SPVC_RESOURCE_TYPE_STAGE_INPUT)
                {
                    if (!graphics || count != graphics->vertex_attributes.size())
                    {
                        return false;
                    }
                    for (std::size_t item = 0; item < count; ++item)
                    {
                        const auto location =
                            spvc_compiler_get_decoration(compiler, list[item].id, SpvDecorationLocation);
                        const auto found =
                            std::ranges::find(graphics->vertex_attributes, location, &VertexAttribute::location);
                        if (found == graphics->vertex_attributes.end())
                        {
                            return false;
                        }
                        const auto format = vertexFormat(found->format);
                        const auto type = spvc_compiler_get_type_handle(compiler, list[item].type_id);
                        const auto scalar = format.scalar == rdesc::EDataScalar::FLOAT  ? SPVC_BASETYPE_FP32
                                            : format.scalar == rdesc::EDataScalar::UINT ? SPVC_BASETYPE_UINT32
                                                                                        : SPVC_BASETYPE_INT32;
                        const bool mismatch = spvc_type_get_basetype(type) != scalar ||
                                              spvc_type_get_vector_size(type) != format.components ||
                                              spvc_type_get_columns(type) != 1 ||
                                              spvc_type_get_num_array_dimensions(type) != 0;
                        if (mismatch)
                        {
                            return false;
                        }
                    }
                }
                else if ((stage == SpvExecutionModelVertex && kind == SPVC_RESOURCE_TYPE_STAGE_OUTPUT) ||
                         (stage == SpvExecutionModelFragment && kind == SPVC_RESOURCE_TYPE_STAGE_INPUT))
                {
                    if (!io)
                    {
                        return false;
                    }
                    auto& fields = kind == SPVC_RESOURCE_TYPE_STAGE_INPUT ? io->inputs : io->outputs;
                    for (std::size_t item = 0; item < count; ++item)
                    {
                        const auto type = spvc_compiler_get_type_handle(compiler, list[item].type_id);
                        const auto scalar = spvc_type_get_basetype(type);
                        const bool numeric = scalar == SPVC_BASETYPE_FP32 || scalar == SPVC_BASETYPE_UINT32 ||
                                             scalar == SPVC_BASETYPE_INT32;
                        const bool invalid = !numeric || spvc_type_get_columns(type) != 1 ||
                                             spvc_type_get_num_array_dimensions(type) != 0;
                        if (invalid)
                        {
                            return false;
                        }
                        fields.push_back(
                            {spvc_compiler_get_decoration(compiler, list[item].id, SpvDecorationLocation),
                             spvc_type_get_vector_size(type),
                             scalar,
                             bool(spvc_compiler_has_decoration(compiler, list[item].id, SpvDecorationFlat))}
                        );
                    }
                }
                else if (stage == SpvExecutionModelFragment && kind == SPVC_RESOURCE_TYPE_STAGE_OUTPUT)
                {
                    if (count != color_count)
                    {
                        return false;
                    }
                    std::vector<bool> locations(color_count, false);
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const auto location = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationLocation);
                        const bool invalid = location >= color_count ||
                                             !float4(spvc_compiler_get_type_handle(compiler, list[i].type_id));
                        if (invalid || locations[location])
                        {
                            return false;
                        }
                        locations[location] = true;
                    }
                }
                else if (count != 0)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    nri::StageBits shaderStages(std::uint32_t stages) noexcept
    {
        auto result = nri::StageBits::ALL; // Zero mask before accumulation, never used as an implicit shader stage.
        if (stages & 1u)
        {
            result |= nri::StageBits::VERTEX_SHADER;
        }
        if (stages & 2u)
        {
            result |= nri::StageBits::FRAGMENT_SHADER;
        }
        if (stages & 4u)
        {
            result |= nri::StageBits::COMPUTE_SHADER;
        }
        return result;
    }

    std::vector<nri::DescriptorRangeDesc> descriptorRanges(
        std::span<const ShaderBinding> contract,
        std::uint32_t image_count,
        std::uint32_t sampler_count
    ) noexcept
    {
        std::vector<nri::DescriptorRangeDesc> ranges;
        ranges.reserve(contract.size());
        for (const auto& binding : contract)
        {
            const auto type = binding.kind == EDescriptorKind::SAMPLED_IMAGE    ? nri::DescriptorType::TEXTURE
                              : binding.kind == EDescriptorKind::SAMPLER        ? nri::DescriptorType::SAMPLER
                              : binding.kind == EDescriptorKind::UNIFORM_BUFFER ? nri::DescriptorType::CONSTANT_BUFFER
                              : binding.kind == EDescriptorKind::STORAGE_IMAGE  ? nri::DescriptorType::STORAGE_TEXTURE
                              : binding.kind == EDescriptorKind::TYPED_STORAGE_BUFFER
                                  ? nri::DescriptorType::STORAGE_BUFFER
                                  : nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
            const bool runtime = binding.count == 0;
            const auto runtime_count = type == nri::DescriptorType::SAMPLER ? sampler_count : image_count;
            const auto flags = runtime ? nri::DescriptorRangeBits::ARRAY | nri::DescriptorRangeBits::PARTIALLY_BOUND |
                                             nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET
                               : binding.count > 1 ? nri::DescriptorRangeBits::ARRAY
                                                   : nri::DescriptorRangeBits::NONE;
            ranges.push_back(
                {binding.binding, runtime ? runtime_count : binding.count, type, shaderStages(binding.stages), flags}
            );
        }
        return ranges;
    }

    RenderResult<nri::Pipeline*> compileCompute(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const EnabledDeviceFeatures& enabled,
        nri::PipelineLayout& layout,
        const rdesc::SpirvArtifact& shader
    ) noexcept
    {
        const bool wrong_stage = shader.stages.size() != 1 || shader.stages[0].stage != rdesc::EShaderType::COMPUTE;
        if (wrong_stage)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto& code = shader.stages[0];
        auto projected = shaderInterface(shader.interface);
        if (!projected)
        {
            return cxx::unexpected(projected.error());
        }
        if (!validateStage(
                code,
                SpvExecutionModelGLCompute,
                *projected,
                nri_core.GetDeviceDesc(device),
                nullptr,
                {},
                0,
                enabled.buffer_device_address
            ))
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const nri::ComputePipelineDesc description{
            &layout,
            {nri::StageBits::COMPUTE_SHADER,
             code.words.data(),
             code.words.size() * sizeof(std::uint32_t),
             code.entry.c_str()}
        };
        nri::Pipeline* pipeline = nullptr;
        const auto result = nri_core.CreateComputePipeline(device, description, pipeline);
        if (result != nri::Result::SUCCESS)
        {
            nri_core.DestroyPipeline(pipeline);
            return cxx::unexpected(nativeError(result));
        }
        return pipeline;
    }

    RenderResult<nri::Pipeline*> compileGraphics(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::PipelineLayout& layout,
        const rdesc::SpirvArtifact& shader,
        const GraphicsState& graphics,
        std::span<const nri::Format> colors,
        nri::Format depth,
        std::uint32_t samples
    ) noexcept
    {
        auto projected = shaderInterface(shader.interface);
        if (!projected)
        {
            return cxx::unexpected(projected.error());
        }
        const bool needs_arrays = std::ranges::any_of(
            projected->bindings,
            [](const auto& value) { return value.kind != EDescriptorKind::STORAGE_BUFFER && value.count == 0; }
        );
        if (needs_arrays && !enabled.texture_descriptors)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        const bool wrong_count =
            shader.stages.empty() || shader.stages.size() > 2 || (!colors.empty() && shader.stages.size() != 2);
        const bool invalid_depth = graphics.depth_compare > EDepthCompare::ALWAYS ||
                                   ((graphics.depth_test || graphics.depth_write) && depth == nri::Format::UNKNOWN);
        const bool invalid_cull = graphics.cull_mode > ECullMode::BACK;
        const bool invalid_bias =
            !std::isfinite(graphics.depth_bias_constant) || !std::isfinite(graphics.depth_bias_slope);
        if (wrong_count || invalid_depth || invalid_cull || invalid_bias)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const rdesc::SpirvStage* vertex_stage = nullptr;
        const rdesc::SpirvStage* fragment_stage = nullptr;
        for (const auto& stage : shader.stages)
        {
            if (stage.stage == rdesc::EShaderType::VERTEX && !vertex_stage)
            {
                vertex_stage = &stage;
            }
            else if (stage.stage == rdesc::EShaderType::FRAGMENT && !fragment_stage)
            {
                fragment_stage = &stage;
            }
            else
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
        }
        if (!vertex_stage || (!colors.empty() && !fragment_stage))
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto& limits = nri_core.GetDeviceDesc(device);
        StageIo vertex_io, fragment_io;
        const bool valid_vertex = validateStage(
            *vertex_stage,
            SpvExecutionModelVertex,
            *projected,
            limits,
            &graphics,
            &vertex_io,
            true,
            enabled.buffer_device_address
        );
        const bool valid_fragment = !fragment_stage || validateStage(
                                                           *fragment_stage,
                                                           SpvExecutionModelFragment,
                                                           *projected,
                                                           limits,
                                                           &graphics,
                                                           &fragment_io,
                                                           static_cast<std::uint32_t>(colors.size())
                                                       );
        if (!valid_vertex || !valid_fragment)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        for (const auto& input : fragment_io.inputs)
        {
            if (std::ranges::find(vertex_io.outputs, input) == vertex_io.outputs.end())
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
        }
        const bool too_many_inputs = graphics.vertex_bindings.size() > limits.shaderStage.vertex.streamMaxNum ||
                                     graphics.vertex_attributes.size() > limits.shaderStage.vertex.attributeMaxNum ||
                                     graphics.vertex_bindings.size() > UINT8_MAX ||
                                     graphics.vertex_attributes.size() > UINT8_MAX;
        if (too_many_inputs)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        std::vector<nri::VertexStreamDesc> streams;
        std::vector<nri::VertexAttributeDesc> attributes;
        for (const auto& binding : graphics.vertex_bindings)
        {
            const bool invalid =
                !binding.stride || binding.stride > vertex_limits[0] || binding.stride > UINT16_MAX ||
                binding.binding >= limits.shaderStage.vertex.streamMaxNum || binding.binding > UINT16_MAX ||
                binding.rate > EVertexRate::INSTANCE ||
                std::ranges::any_of(streams, [&](const auto& other) { return other.bindingSlot == binding.binding; });
            if (invalid)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            streams.push_back(
                {static_cast<std::uint16_t>(binding.binding),
                 binding.rate == EVertexRate::VERTEX ? nri::VertexStreamStepRate::PER_VERTEX
                                                     : nri::VertexStreamStepRate::PER_INSTANCE,
                 static_cast<std::uint16_t>(binding.stride)}
            );
        }
        for (const auto& field : graphics.vertex_attributes)
        {
            const auto format = vertexFormat(field.format);
            const auto binding = std::ranges::find(streams, field.binding, &nri::VertexStreamDesc::bindingSlot);
            const bool invalid =
                !format.components || field.location >= limits.shaderStage.vertex.attributeMaxNum ||
                field.offset > vertex_limits[1] || binding == streams.end() ||
                std::ranges::any_of(attributes, [&](const auto& other) { return other.vk.location == field.location; });
            if (invalid)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            if (std::uint64_t(field.offset) + format.bytes() > binding->stride)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            if ((formats[static_cast<std::size_t>(format.native)] & nri::FormatSupportBits::VERTEX_BUFFER) == 0)
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            }
            attributes.push_back(
                {{},
                 {field.location},
                 field.offset,
                 format.native,
                 static_cast<std::uint16_t>(binding - streams.begin())}
            );
        }
        const std::array code{vertex_stage, fragment_stage};
        std::array<nri::ShaderDesc, 2> stages{};
        const auto stage_count = fragment_stage ? 2u : 1u;
        for (std::size_t i = 0; i < stage_count; ++i)
        {
            stages[i] = {
                i == 0 ? nri::StageBits::VERTEX_SHADER : nri::StageBits::FRAGMENT_SHADER,
                code[i]->words.data(),
                code[i]->words.size() * sizeof(std::uint32_t),
                code[i]->entry.c_str()
            };
        }
        const nri::VertexInputDesc input{
            attributes.data(),
            static_cast<std::uint8_t>(attributes.size()),
            streams.data(),
            static_cast<std::uint8_t>(streams.size())
        };
        std::vector<nri::ColorAttachmentDesc> attachments;
        for (auto color : colors)
        {
            nri::ColorAttachmentDesc attachment{};
            attachment.format = color;
            attachment.colorWriteMask = nri::ColorWriteBits::RGBA;
            attachments.push_back(attachment);
        }
        constexpr std::array comparisons{
            nri::CompareOp::NEVER,
            nri::CompareOp::LESS,
            nri::CompareOp::EQUAL,
            nri::CompareOp::LESS_EQUAL,
            nri::CompareOp::GREATER,
            nri::CompareOp::NOT_EQUAL,
            nri::CompareOp::GREATER_EQUAL,
            nri::CompareOp::ALWAYS
        };
        nri::GraphicsPipelineDesc description{};
        description.pipelineLayout = &layout;
        description.vertexInput = streams.empty() ? nullptr : &input;
        description.inputAssembly.topology = nri::Topology::TRIANGLE_LIST;
        constexpr std::array cull_modes{nri::CullMode::NONE, nri::CullMode::FRONT, nri::CullMode::BACK};
        description.rasterization.frontCounterClockwise = graphics.front_counter_clockwise;
        description.rasterization.cullMode = cull_modes[static_cast<std::size_t>(graphics.cull_mode)];
        description.rasterization.depthBias = {graphics.depth_bias_constant, 0, graphics.depth_bias_slope};
        const nri::MultisampleDesc multisample{UINT32_MAX, static_cast<nri::Sample_t>(samples)};
        description.multisample = samples > 1 ? &multisample : nullptr;
        description.outputMerger.colors = attachments.data();
        description.outputMerger.colorNum = static_cast<std::uint32_t>(attachments.size());
        description.outputMerger.depthStencilFormat = depth;
        description.outputMerger.depth.compareOp = graphics.depth_test
                                                       ? comparisons[static_cast<unsigned>(graphics.depth_compare)]
                                                   : graphics.depth_write ? nri::CompareOp::ALWAYS
                                                                          : nri::CompareOp::NONE;
        description.outputMerger.depth.write = graphics.depth_write;
        description.shaders = stages.data();
        description.shaderNum = stage_count;
        nri::Pipeline* pipeline = nullptr;
        const auto result = nri_core.CreateGraphicsPipeline(device, description, pipeline);
        if (result != nri::Result::SUCCESS)
        {
            nri_core.DestroyPipeline(pipeline);
            return cxx::unexpected(nativeError(result));
        }
        return pipeline;
    }
} // namespace lux::render::detail
