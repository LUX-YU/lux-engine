#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/compile/GraphDefinition.hpp>
#include <lux/engine/render/graph/Shader.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/graph/RasterProgram.hpp>
#include <memory>
#include <optional>
#include <spirv-tools/libspirv.h>
#include <spirv_cross_c.h>

namespace lux::render::detail
{
    VertexFormat vertexFormat(rdesc::ETextureFormat format) noexcept
    {
        using F = rdesc::ETextureFormat;
        using S = rdesc::EDataScalar;
        switch (format)
        {
        case F::RGBA8_UNORM:
            return {nri::Format::RGBA8_UNORM, S::FLOAT, 4, 1};
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

        bool matchesMatrixStorage(spvc_compiler compiler, spvc_type type, const rdesc::ShaderMember& field) noexcept
        {
            if (spvc_type_get_num_member_types(type) != 1)
            {
                return false;
            }
            const auto columns = spvc_compiler_get_type_handle(compiler, spvc_type_get_member_type(type, 0));
            const bool has_columns =
                spvc_type_get_num_array_dimensions(columns) == 1 && spvc_type_array_dimension_is_literal(columns, 0);
            if (!has_columns)
            {
                return false;
            }
            unsigned offset = 0, stride = 0;
            const bool has_layout = check(spvc_compiler_type_struct_member_offset(compiler, type, 0, &offset)) &&
                                    check(spvc_compiler_type_struct_member_array_stride(compiler, type, 0, &stride));
            const bool is_float_matrix = field.scalar == rdesc::EDataScalar::FLOAT &&
                                         spvc_type_get_basetype(columns) == SPVC_BASETYPE_FP32 &&
                                         spvc_type_get_bit_width(columns) == 32;
            const bool has_matching_columns = spvc_type_get_array_dimension(columns, 0) == field.columns &&
                                              spvc_type_get_vector_size(columns) == field.rows &&
                                              spvc_type_get_columns(columns) == 1;
            const bool order_matches = field.matrix_order == rdesc::EShaderMatrixOrder::UNSPECIFIED ||
                field.matrix_order == rdesc::EShaderMatrixOrder::COLUMN_MAJOR;
            return has_layout && is_float_matrix && has_matching_columns && order_matches && offset == 0 &&
                   stride == field.matrix_stride;
        }

        // One depth-first walk compares reflected members in declaration order, including nested arrays.
        class SpirvBlockReader final
        {
        public:
            SpirvBlockReader(spvc_compiler compiler, const rdesc::ShaderBlock& expected) noexcept
                : compiler_(compiler), expected_(expected)
            {
            }

            bool matches(spvc_type record) noexcept
            {
                return readFields(record, "", 0) && cursor_ == expected_.fields.size();
            }

        private:
            bool readFields(spvc_type type, const std::string& path, std::uint64_t base, bool array_wrapper = false) noexcept;

            spvc_compiler compiler_;
            const rdesc::ShaderBlock& expected_;
            std::vector<rdesc::ShaderArrayDimension> array_shape_;
            std::size_t cursor_ = 0;
        };

        bool SpirvBlockReader::readFields(
            spvc_type type, const std::string& path, std::uint64_t base, bool array_wrapper
        ) noexcept
        {
            const auto structure_id = spvc_type_get_base_type_id(type);
            for (unsigned member_index = 0; member_index < spvc_type_get_num_member_types(type); ++member_index)
            {
                const auto member =
                    spvc_compiler_get_type_handle(compiler_, spvc_type_get_member_type(type, member_index));
                unsigned member_offset = 0;
                const auto offset_result =
                    spvc_compiler_type_struct_member_offset(compiler_, type, member_index, &member_offset);
                if (!check(offset_result))
                {
                    return false;
                }
                const auto offset = base + member_offset;
                if (offset > expected_.size)
                {
                    return false;
                }
                const auto name =
                    array_wrapper ? path : path + spvc_compiler_get_member_name(compiler_, structure_id, member_index);
                const auto dimensions = spvc_type_get_num_array_dimensions(member);
                unsigned count = 1, stride = array_shape_.empty() ? 0 : array_shape_.back().stride;
                if (dimensions)
                {
                    if (dimensions != 1 || !spvc_type_array_dimension_is_literal(member, 0))
                    {
                        return false;
                    }
                    count = spvc_type_get_array_dimension(member, 0);
                    if (!count || count > expected_.fields.size())
                    {
                        return false;
                    }
                    if (!check(spvc_compiler_type_struct_member_array_stride(compiler_, type, member_index, &stride)))
                    {
                        return false;
                    }
                    array_shape_.push_back({count, stride});
                }
                for (unsigned item = 0; item < count; ++item)
                {
                    const auto at = offset + (dimensions ? std::uint64_t(item) * stride : 0);
                    if (at > expected_.size)
                    {
                        return false;
                    }
                    const auto item_name = name + (dimensions ? "[" + std::to_string(item) + "]" : "");
                    const auto scalar = spvc_type_get_basetype(member);
                    if (scalar == SPVC_BASETYPE_STRUCT)
                    {
                        const bool has_expected_field = cursor_ < expected_.fields.size();
                        const bool is_matrix = has_expected_field && expected_.fields[cursor_].path == item_name &&
                                               expected_.fields[cursor_].columns > 1;
                        if (is_matrix)
                        {
                            const auto& field = expected_.fields[cursor_++];
                            const bool has_matching_shape =
                                field.matrix_order == rdesc::EShaderMatrixOrder::UNSPECIFIED ||
                                field.array_dimensions == array_shape_;
                            const bool has_matching_position =
                                field.offset == at && field.array_stride == stride && has_matching_shape;
                            if (!has_matching_position || !matchesMatrixStorage(compiler_, member, field))
                            {
                                return false;
                            }
                            continue;
                        }
                        // Slang represents a fixed array as a one-member struct containing its data array.
                        // Only unwrap when the target metadata describes that exact array path and zero offset.
                        unsigned wrapper_offset = 0;
                        const bool has_array_path =
                            has_expected_field && expected_.fields[cursor_].path.starts_with(item_name + "[");
                        const bool is_array_wrapper =
                            has_array_path && spvc_type_get_num_member_types(member) == 1 &&
                            check(spvc_compiler_type_struct_member_offset(compiler_, member, 0, &wrapper_offset)) &&
                            wrapper_offset == 0;
                        const bool valid = readFields(member, item_name + (is_array_wrapper ? "" : "."), at, is_array_wrapper);
                        if (!valid)
                        {
                            return false;
                        }
                        continue;
                    }
                    const bool numeric =
                        scalar == SPVC_BASETYPE_FP32 || scalar == SPVC_BASETYPE_INT32 || scalar == SPVC_BASETYPE_UINT32;
                    const bool row_major = spvc_compiler_has_member_decoration(
                        compiler_,
                        structure_id,
                        member_index,
                        SpvDecorationRowMajor
                    );
                    const bool invalid = !numeric || spvc_type_get_bit_width(member) != 32 || row_major ||
                                         cursor_ >= expected_.fields.size();
                    if (invalid)
                    {
                        return false;
                    }
                    unsigned matrix_stride = 0;
                    if (spvc_type_get_columns(member) > 1)
                    {
                        const auto result = spvc_compiler_type_struct_member_matrix_stride(
                            compiler_,
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
                    const auto& field = expected_.fields[cursor_++];
                    rdesc::ShaderMember actual{
                        item_name,
                        scalar_kind,
                        static_cast<std::uint32_t>(at),
                        spvc_type_get_vector_size(member),
                        spvc_type_get_columns(member),
                        stride,
                        matrix_stride,
                        rdesc::EShaderMatrixOrder::UNSPECIFIED,
                        {}
                    };
                    if (field.matrix_order != rdesc::EShaderMatrixOrder::UNSPECIFIED)
                    {
                        const bool is_matrix = spvc_type_get_columns(member) > 1;
                        if (is_matrix && !spvc_compiler_has_member_decoration(
                                             compiler_, structure_id, member_index, SpvDecorationColMajor
                                         ))
                        {
                            return false;
                        }
                        actual.matrix_order = is_matrix ? rdesc::EShaderMatrixOrder::COLUMN_MAJOR
                                                        : rdesc::EShaderMatrixOrder::NOT_MATRIX;
                        actual.array_dimensions = array_shape_;
                    }
                    if (field != actual)
                    {
                        return false;
                    }
                }
                if (dimensions) array_shape_.pop_back();
            }
            return true;
        }

        bool validateBlock(spvc_compiler compiler, spvc_type block, const rdesc::ShaderBlock& expected) noexcept
        {
            if (!expected.hasStructuredLayout())
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
            std::size_t size = 0;
            if (!check(spvc_compiler_get_declared_struct_size(compiler, record, &size)))
            {
                return false;
            }
            const auto rounded =
                (std::uint64_t(size) + expected.alignment - 1) / expected.alignment * expected.alignment;
            // ArrayStride was checked against SPIR-V above. A runtime record can have trailing
            // padding that is absent from its declared member extent; do not infer that ABI again.
            const bool size_matches =
                expected.runtime_records
                    ? size <= expected.size
                    : (rounded == expected.size ||
                       (expected.storage == rdesc::EDataLayout::TARGET_REFLECTED && size == expected.size));
            return size_matches && SpirvBlockReader{compiler, expected}.matches(record);
        }

        std::optional<StageIo> validateStage(
            const DeviceSession& session,
            const rdesc::ShaderStageArtifact& input,
            const ShaderInterface& interface,
            const RasterProgramKey* raster = nullptr
        ) noexcept
        {
            SpvExecutionModel stage;
            switch (input.stage)
            {
            case rdesc::EShaderType::VERTEX: stage = SpvExecutionModelVertex; break;
            case rdesc::EShaderType::FRAGMENT: stage = SpvExecutionModelFragment; break;
            case rdesc::EShaderType::COMPUTE: stage = SpvExecutionModelGLCompute; break;
            default: return std::nullopt;
            }
            const auto& limits = session.description();
            const auto* graphics = raster ? &raster->state : nullptr;
            const auto color_count = raster ? raster->colors.size() : 0;
            const bool device_address = stage != SpvExecutionModelFragment && session.enabledFeatures().buffer_device_address;
            StageIo io;
            const bool invalid_entry = input.entry.empty() || input.entry.find('\0') != std::string::npos;
            if (invalid_entry)
            {
                return std::nullopt;
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
                return std::nullopt;
            }
            spvc_context raw{};
            if (!check(spvc_context_create(&raw)))
            {
                return std::nullopt;
            }
            const std::unique_ptr<spvc_context_s, decltype(&spvc_context_destroy)> context(raw, spvc_context_destroy);
            spvc_parsed_ir ir{};
            spvc_compiler compiler{};
            spvc_resources resources{};
            if (!check(spvc_context_parse_spirv(raw, words.data(), words.size(), &ir)))
            {
                return std::nullopt;
            }
            if (!check(spvc_context_create_compiler(
                    raw,
                    SPVC_BACKEND_NONE,
                    ir,
                    SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                    &compiler
                )))
            {
                return std::nullopt;
            }
            const SpvCapability* capabilities{};
            std::size_t capability_count = 0;
            if (!check(spvc_compiler_get_declared_capabilities(compiler, &capabilities, &capability_count)))
            {
                return std::nullopt;
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
                const bool storage_array =
                    std::ranges::any_of(
                        interface.bindings,
                        [](const auto& value) noexcept
                        { return value.kind == EDescriptorKind::STORAGE_BUFFER && value.count == 0; }
                    ) &&
                    (capability == SpvCapabilityRuntimeDescriptorArray || capability == SpvCapabilityShaderNonUniform ||
                     capability == SpvCapabilityStorageBufferArrayNonUniformIndexing ||
                     capability == SpvCapabilityStorageBufferArrayDynamicIndexing);
                const bool physical_addresses =
                    device_address && capability == SpvCapabilityPhysicalStorageBufferAddresses;
                const bool integer64 = limits.shaderFeatures.nativeI64 && capability == SpvCapabilityInt64;
                const bool draw_parameters =
                    limits.shaderFeatures.drawParameters && capability == SpvCapabilityDrawParameters;
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
                const bool is_unsupported_capability = capability != SpvCapabilityShader && !sampled &&
                                                       !storage_array && !physical_addresses && !image_query &&
                                                       !integer64 && !draw_parameters;
                if (is_unsupported_capability)
                {
                    return std::nullopt;
                }
            }
            const spvc_entry_point* entries{};
            std::size_t count = 0;
            if (!check(spvc_compiler_get_entry_points(compiler, &entries, &count)))
            {
                return std::nullopt;
            }
            const bool is_entry_mismatch =
                count != 1 || entries[0].execution_model != stage || input.entry != entries[0].name;
            if (is_entry_mismatch)
            {
                return std::nullopt;
            }
            if (stage == SpvExecutionModelGLCompute)
            {
                const SpvExecutionMode* modes{};
                std::size_t mode_count = 0;
                if (!check(spvc_compiler_get_execution_modes(compiler, &modes, &mode_count)))
                {
                    return std::nullopt;
                }
                const auto mode_list = std::span(modes, mode_count);
                const bool has_local = std::ranges::find(mode_list, SpvExecutionModeLocalSize) != mode_list.end();
                const bool specialized = std::ranges::find(mode_list, SpvExecutionModeLocalSizeId) != mode_list.end();
                if (!has_local && !specialized)
                {
                    return std::nullopt;
                }
                spvc_specialization_constant sizes[3]{};
                spvc_compiler_get_work_group_size_specialization_constants(compiler, &sizes[0], &sizes[1], &sizes[2]);
                if (specialized && (sizes[0].id || sizes[1].id || sizes[2].id))
                {
                    return std::nullopt;
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
                        return std::nullopt;
                    }
                    invocations *= size;
                    if (invocations > limits.shaderStage.compute.workGroupInvocationMaxNum)
                    {
                        return std::nullopt;
                    }
                }
            }
            if (!check(spvc_compiler_create_shader_resources(compiler, &resources)))
            {
                return std::nullopt;
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
                    return std::nullopt;
                }
                if (kind == SPVC_RESOURCE_TYPE_PUSH_CONSTANT)
                {
                    const bool declared = (interface.parameter_stages & stage_bit) != 0;
                    if (count != unsigned(declared))
                    {
                        return std::nullopt;
                    }
                    if (declared)
                    {
                        const auto structure = spvc_compiler_get_type_handle(compiler, list[0].base_type_id);
                        if (!validateBlock(compiler, structure, interface.parameters))
                        {
                            return std::nullopt;
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
                        return std::nullopt;
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
                            return std::nullopt;
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
                                return std::nullopt;
                            }
                        }
                        else if (expected_kind == EDescriptorKind::SAMPLER)
                        {
                            const auto dimensions = spvc_type_get_num_array_dimensions(type);
                            if (dimensions == 0
                                    ? expected->count != 1
                                    : dimensions != 1 || spvc_type_get_array_dimension(type, 0) != expected->count)
                            {
                                return std::nullopt;
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
                                return std::nullopt;
                            }
                            const auto flags = std::span(decorations, decoration_count);
                            const bool read = std::ranges::find(flags, SpvDecorationNonReadable) == flags.end();
                            const bool write = std::ranges::find(flags, SpvDecorationNonWritable) == flags.end();
                            const bool access_mismatch = read != (expected->access != EShaderAccess::WRITE_ONLY) ||
                                                         write != (expected->access != EShaderAccess::READ_ONLY);
                            if (expected_kind == EDescriptorKind::STORAGE_BUFFER && access_mismatch)
                            {
                                return std::nullopt;
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
                                return std::nullopt;
                            }
                        }
                    }
                }
                else if (stage == SpvExecutionModelVertex && kind == SPVC_RESOURCE_TYPE_STAGE_INPUT)
                {
                    if (!graphics || count != graphics->vertex_attributes.size())
                    {
                        return std::nullopt;
                    }
                    for (std::size_t item = 0; item < count; ++item)
                    {
                        const auto location =
                            spvc_compiler_get_decoration(compiler, list[item].id, SpvDecorationLocation);
                        const auto found =
                            std::ranges::find(graphics->vertex_attributes, location, &VertexAttribute::location);
                        if (found == graphics->vertex_attributes.end())
                        {
                            return std::nullopt;
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
                            return std::nullopt;
                        }
                    }
                }
                else if ((stage == SpvExecutionModelVertex && kind == SPVC_RESOURCE_TYPE_STAGE_OUTPUT) ||
                         (stage == SpvExecutionModelFragment && kind == SPVC_RESOURCE_TYPE_STAGE_INPUT))
                {
                    auto& fields = kind == SPVC_RESOURCE_TYPE_STAGE_INPUT ? io.inputs : io.outputs;
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
                            return std::nullopt;
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
                        return std::nullopt;
                    }
                    std::vector<bool> locations(color_count, false);
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const auto location = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationLocation);
                        const bool invalid = location >= color_count ||
                                             !float4(spvc_compiler_get_type_handle(compiler, list[i].type_id));
                        if (invalid || locations[location])
                        {
                            return std::nullopt;
                        }
                        locations[location] = true;
                    }
                }
                else if (count != 0)
                {
                    return std::nullopt;
                }
            }
            return io;
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

    std::vector<nri::DescriptorRangeDesc>
    descriptorRanges(std::span<const ShaderBinding> contract, bool preserve_registers) noexcept
    {
        std::vector<nri::DescriptorRangeDesc> ranges;
        ranges.reserve(contract.size());
        for (const auto& binding : contract)
        {
            const auto type =
                binding.kind == EDescriptorKind::SAMPLED_IMAGE          ? nri::DescriptorType::TEXTURE
                : binding.kind == EDescriptorKind::SAMPLER              ? nri::DescriptorType::SAMPLER
                : binding.kind == EDescriptorKind::UNIFORM_BUFFER       ? nri::DescriptorType::CONSTANT_BUFFER
                : binding.kind == EDescriptorKind::STORAGE_IMAGE        ? nri::DescriptorType::STORAGE_TEXTURE
                : binding.kind == EDescriptorKind::TYPED_STORAGE_BUFFER ? nri::DescriptorType::STORAGE_BUFFER
                : binding.access == EShaderAccess::READ_ONLY            ? nri::DescriptorType::STRUCTURED_BUFFER
                                                             : nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
            const auto flags = binding.count > 1 ? nri::DescriptorRangeBits::ARRAY : nri::DescriptorRangeBits::NONE;
            ranges.push_back(
                {binding.binding,
                 binding.count,
                 type,
                 shaderStages(binding.stages),
                 flags,
                 preserve_registers ? binding.shader_space + 1 : 0,
                 binding.shader_register}
            );
        }
        return ranges;
    }

    namespace
    {
        bool supportsDescriptorArrays(const ShaderInterface& shader, const EnabledDeviceFeatures& enabled) noexcept
        {
            for (const auto& binding : shader.bindings)
            {
                if (binding.count != 0)
                {
                    continue;
                }
                const bool buffer = binding.kind == EDescriptorKind::STORAGE_BUFFER;
                const bool supported = buffer ? enabled.buffer_descriptors : enabled.texture_descriptors;
                if (!supported)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    RenderResult<nri::Pipeline*>
    compileCompute(
        const DeviceSession& session, nri::PipelineLayout& layout, const rdesc::ShaderProgramArtifact& shader
    ) noexcept
    {
        const auto& enabled = session.enabledFeatures();
        const auto& nri_core = session.nriCore();
        const bool wrong_stage = shader.stages.size() != 1 || shader.stages[0].stage != rdesc::EShaderType::COMPUTE;
        if (shader.target != enabled.shader_target)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
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
        if (!supportsDescriptorArrays(*projected, enabled))
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        if (shader.target == rdesc::EShaderTarget::SPIRV && !validateStage(session, code, *projected))
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (!rdesc::validShaderContainer(shader.target, code.words))
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
        const auto result = nri_core.CreateComputePipeline(session.device(), description, pipeline);
        if (result != nri::Result::SUCCESS)
        {
            nri_core.DestroyPipeline(pipeline);
            return cxx::unexpected(nativeError(result));
        }
        return pipeline;
    }

    RenderResult<std::array<const rdesc::ShaderStageArtifact*, 2>>
    rasterShaderStages(
        const DeviceSession& session, const rdesc::ShaderProgramArtifact& shader, const RasterProgramKey& program
    ) noexcept
    {
        const auto& graphics = program.state;
        const auto& enabled = session.enabledFeatures();
        const auto color_count = program.colors.size();
        const auto depth = program.depth;
        if (shader.target != enabled.shader_target)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        auto projected = shaderInterface(shader.interface);
        if (!projected)
        {
            return cxx::unexpected(projected.error());
        }
        if (!supportsDescriptorArrays(*projected, enabled))
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        const bool wrong_count =
            shader.stages.empty() || shader.stages.size() > 2 || (color_count != 0 && shader.stages.size() != 2);
        const bool invalid_depth = graphics.depth_compare > EDepthCompare::ALWAYS ||
                                   ((graphics.depth_test || graphics.depth_write) && depth == nri::Format::UNKNOWN);
        const bool invalid_cull = graphics.cull_mode > ECullMode::BACK;
        const bool invalid_bias =
            !std::isfinite(graphics.depth_bias_constant) || !std::isfinite(graphics.depth_bias_slope);
        if (wrong_count || invalid_depth || invalid_cull || invalid_bias)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const rdesc::ShaderStageArtifact* vertex_stage = nullptr;
        const rdesc::ShaderStageArtifact* fragment_stage = nullptr;
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
        if (!vertex_stage || (color_count != 0 && !fragment_stage))
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (shader.target == rdesc::EShaderTarget::DXIL)
        {
            for (const auto& stage : shader.stages)
            {
                if (!rdesc::validShaderContainer(shader.target, stage.words))
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
            }
            const auto vertex = std::ranges::find(
                shader.interface.entry_points,
                rdesc::EShaderType::VERTEX,
                &rdesc::EntryPointInfo::stage
            );
            if (vertex == shader.interface.entry_points.end() ||
                vertex->inputs.size() != graphics.vertex_attributes.size())
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            for (const auto& attribute : graphics.vertex_attributes)
            {
                const auto input =
                    std::ranges::find(vertex->inputs, attribute.location, &rdesc::ShaderIoVariable::location);
                const auto format = vertexFormat(attribute.format);
                const auto scalar = format.scalar == rdesc::EDataScalar::FLOAT
                                        ? rdesc::EVertexScalarBase::FLOAT
                                        : (format.scalar == rdesc::EDataScalar::UINT ? rdesc::EVertexScalarBase::UINT
                                                                                     : rdesc::EVertexScalarBase::INT);
                const bool has_input = input != vertex->inputs.end();
                const bool is_matching = has_input && input->base == scalar && input->vec_size == format.components &&
                                         input->columns == 1 && input->array_size == 1;
                if (!is_matching)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
            }
            // The D3D12 PSO compiler validates the actual DXIL stage signatures and root signature together.
            return std::array{vertex_stage, fragment_stage};
        }
        const auto vertex_io = validateStage(session, *vertex_stage, *projected, &program);
        if (!vertex_io) return cxx::unexpected(RenderError{kDeviceArgument});
        if (fragment_stage)
        {
            const auto fragment_io = validateStage(session, *fragment_stage, *projected, &program);
            if (!fragment_io) return cxx::unexpected(RenderError{kDeviceArgument});
            for (const auto& input : fragment_io->inputs)
            {
                if (std::ranges::find(vertex_io->outputs, input) == vertex_io->outputs.end())
                    return cxx::unexpected(RenderError{kDeviceArgument});
            }
        }
        return std::array{vertex_stage, fragment_stage};
    }
} // namespace lux::render::detail
