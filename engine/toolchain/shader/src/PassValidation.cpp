#include <algorithm>
#include <charconv>
#include <lux/engine/toolchain/shader/PassValidation.hpp>
#include <lux/engine/toolchain/shader/SpirvReflection.hpp>
#include <new>
#include <optional>
#include <spirv_cross.hpp>
#include <vector>

namespace lux::toolchain
{
    namespace
    {
        namespace sc = SPIRV_CROSS_NAMESPACE;
        using ERole = rdesc::EPassFieldRole;

        bool shaderResource(ERole role) noexcept
        {
            return role <= ERole::READ_WRITE_STORAGE || role == ERole::INPUT_ATTACHMENT;
        }

        rdesc::EDescriptorType descriptorType(ERole role) noexcept
        {
            using EType = rdesc::EDescriptorType;
            switch (role)
            {
            case ERole::SAMPLED_READ:
                return EType::SAMPLED_IMAGE;
            case ERole::SAMPLER:
                return EType::SAMPLER;
            case ERole::UNIFORM_READ:
                return EType::UNIFORM_BUFFER;
            case ERole::READ_ONLY_STORAGE:
            case ERole::READ_WRITE_STORAGE:
                return EType::STORAGE_BUFFER;
            case ERole::INPUT_ATTACHMENT:
                return EType::INPUT_ATTACHMENT;
            default:
                return EType::STORAGE_IMAGE;
            }
        }

        struct ScalarLocation
        {
            std::uint32_t offset;
            const sc::SPIRType* type;
        };

        std::optional<ScalarLocation> locate(
            const sc::Compiler& compiler,
            const sc::SPIRType& type,
            std::string_view path,
            std::uint32_t base = 0
        )
        {
            const auto end = path.find_first_of(".[");
            const auto name = path.substr(0, end);
            for (std::uint32_t i = 0; i < type.member_types.size(); ++i)
            {
                if (compiler.get_member_name(type.self, i) != name)
                {
                    continue;
                }
                const auto& member = compiler.get_type(type.member_types[i]);
                auto offset = base + compiler.type_struct_member_offset(type, i);
                auto tail = end == std::string_view::npos ? std::string_view{} : path.substr(end);
                if (tail.starts_with("["))
                {
                    const auto close = tail.find(']');
                    if (close == std::string_view::npos || member.array.empty())
                    {
                        return {};
                    }
                    std::uint32_t index{};
                    const auto parsed = std::from_chars(tail.data() + 1, tail.data() + close, index);
                    if (parsed.ec != std::errc{} || parsed.ptr != tail.data() + close)
                    {
                        return {};
                    }
                    const bool is_fixed = member.array_size_literal[0] && member.array[0] != 0;
                    if (is_fixed && index >= member.array[0])
                    {
                        return {};
                    }
                    offset += index * compiler.type_struct_member_array_stride(type, i);
                    tail.remove_prefix(close + 1);
                }
                if (tail.empty())
                {
                    return ScalarLocation{offset, &member};
                }
                if (!tail.starts_with(".") || member.basetype != sc::SPIRType::Struct)
                {
                    return {};
                }
                tail.remove_prefix(1);
                return locate(compiler, member, tail, offset);
            }
            return {};
        }

        std::size_t scalarCount(const sc::Compiler& compiler, const sc::SPIRType& type)
        {
            std::size_t count = 1;
            for (const auto length : type.array)
            {
                count *= length == 0 ? 1 : length;
            }
            if (type.basetype != sc::SPIRType::Struct)
            {
                return count;
            }
            std::size_t members = 0;
            for (const auto id : type.member_types)
            {
                members += scalarCount(compiler, compiler.get_type(id));
            }
            return count * members;
        }

        cxx::expected<void, std::string> validate(
            std::span<const std::uint32_t> words,
            const rdesc::PassShaderContract& contract
        )
        {
            rdesc::ShaderInfo reflected;
            if (!reflectSpirv(words.data(), words.size_bytes(), reflected))
            {
                return cxx::unexpected(std::string("SPIR-V reflection failed"));
            }
            sc::Compiler compiler(words.data(), words.size());
            const auto model = compiler.get_execution_model();
            const bool supported_stage = model == spv::ExecutionModelGLCompute || model == spv::ExecutionModelVertex ||
                                         model == spv::ExecutionModelFragment;
            if (!supported_stage)
            {
                return cxx::unexpected(std::string("Unsupported Shader stage"));
            }
            const auto stage_bit =
                model == spv::ExecutionModelGLCompute ? 4u : (model == spv::ExecutionModelVertex ? 1u : 2u);
            const auto native = compiler.get_shader_resources();
            std::vector<sc::Resource> resources;
            for (const auto& list :
                 {native.separate_images,
                  native.separate_samplers,
                  native.sampled_images,
                  native.storage_images,
                  native.uniform_buffers,
                  native.storage_buffers,
                  native.subpass_inputs})
            {
                resources.insert(resources.end(), list.begin(), list.end());
            }
            std::uint32_t binding = 0;
            std::uint32_t active_bindings = 0;
            for (const auto& field : contract.resources)
            {
                if (!shaderResource(field.role))
                {
                    continue;
                }
                if ((field.stages & stage_bit) == 0)
                {
                    ++binding;
                    continue;
                }
                ++active_bindings;
                const rdesc::EDescriptorBindingInfo* actual = nullptr;
                for (const auto& set : reflected.sets)
                {
                    for (const auto& candidate : set.bindings)
                    {
                        if (set.set == 0 && candidate.binding == binding)
                        {
                            actual = &candidate;
                        }
                    }
                }
                const bool is_invalid_binding = actual == nullptr || actual->type != descriptorType(field.role) ||
                                                actual->count != field.array_count;
                if (is_invalid_binding)
                {
                    return cxx::unexpected("Descriptor type/count mismatch: " + std::string(field.path));
                }
                const bool is_storage = actual->type == rdesc::EDescriptorType::STORAGE_BUFFER ||
                                        actual->type == rdesc::EDescriptorType::STORAGE_IMAGE;
                const bool expects_write = field.role == ERole::STORAGE_WRITE ||
                                           field.role == ERole::STORAGE_READ_WRITE ||
                                           field.role == ERole::READ_WRITE_STORAGE;
                if (is_storage && actual->writable != expects_write)
                {
                    return cxx::unexpected("Resource access mismatch: " + std::string(field.path));
                }
                const sc::Resource* resource = nullptr;
                for (const auto& candidate : resources)
                {
                    const bool is_match = compiler.get_decoration(candidate.id, spv::DecorationDescriptorSet) == 0 &&
                                          compiler.get_decoration(candidate.id, spv::DecorationBinding) == binding;
                    if (is_match)
                    {
                        resource = &candidate;
                    }
                }
                if (resource == nullptr)
                {
                    return cxx::unexpected("Missing resource: " + std::string(field.path));
                }
                if (compiler.get_name(resource->id) != field.shader_name)
                {
                    return cxx::unexpected("Resource identity mismatch: " + std::string(field.path));
                }
                if (actual->type == rdesc::EDescriptorType::STORAGE_IMAGE)
                {
                    const bool non_readable = compiler.has_decoration(resource->id, spv::DecorationNonReadable);
                    if (non_readable != (field.role == ERole::STORAGE_WRITE))
                    {
                        return cxx::unexpected("Resource read access mismatch: " + std::string(field.path));
                    }
                }
                if (field.element_stride != 0 && field.role != ERole::UNIFORM_READ)
                {
                    const auto& block = compiler.get_type(resource->base_type_id);
                    if (block.member_types.size() != 1)
                    {
                        return cxx::unexpected("Storage block shape mismatch: " + std::string(field.path));
                    }
                    const auto& elements = compiler.get_type(block.member_types[0]);
                    if (elements.array.size() != 1 || elements.array[0] != 0)
                    {
                        return cxx::unexpected("Storage runtime array mismatch: " + std::string(field.path));
                    }
                    if (compiler.type_struct_member_array_stride(block, 0) != field.element_stride)
                    {
                        return cxx::unexpected("Storage stride mismatch: " + std::string(field.path));
                    }
                }
                const auto& type = compiler.get_type(resource->type_id);
                const bool is_array_mismatch =
                    field.descriptor_array != !type.array.empty() || type.array.size() > 1 ||
                    (!type.array.empty() && (!type.array_size_literal[0] || type.array[0] != field.array_count));
                if (is_array_mismatch)
                {
                    return cxx::unexpected("Descriptor array shape mismatch: " + std::string(field.path));
                }
                if (field.role == ERole::UNIFORM_READ)
                {
                    const auto& block = compiler.get_type(resource->base_type_id);
                    if (compiler.get_declared_struct_size(block) != field.element_stride)
                    {
                        return cxx::unexpected("Uniform block size mismatch: " + std::string(field.path));
                    }
                }
                if (actual->type == rdesc::EDescriptorType::STORAGE_BUFFER &&
                    compiler.get_buffer_block_flags(resource->id).get(spv::DecorationNonReadable))
                {
                    return cxx::unexpected("Storage buffer read access mismatch: " + std::string(field.path));
                }
                const bool is_image = actual->type == rdesc::EDescriptorType::SAMPLED_IMAGE ||
                                      actual->type == rdesc::EDescriptorType::STORAGE_IMAGE;
                if (is_image)
                {
                    const auto expected_dim = field.dimension.starts_with("1D")     ? spv::Dim1D
                                              : field.dimension.starts_with("3D")   ? spv::Dim3D
                                              : field.dimension.starts_with("Cube") ? spv::DimCube
                                                                                    : spv::Dim2D;
                    const bool is_wrong_dimension =
                        type.image.dim != expected_dim || type.image.arrayed != field.dimension.ends_with("Array") ||
                        type.image.ms != (field.dimension.find("MS") != std::string_view::npos);
                    if (is_wrong_dimension)
                    {
                        return cxx::unexpected("Image dimension mismatch: " + std::string(field.path));
                    }
                }
                if (actual->type == rdesc::EDescriptorType::STORAGE_IMAGE)
                {
                    const bool is_wrong_format =
                        (field.image_format == "rgba32f" && type.image.format != spv::ImageFormatRgba32f) ||
                        (field.image_format == "rgba16f" && type.image.format != spv::ImageFormatRgba16f) ||
                        (field.image_format == "r32f" && type.image.format != spv::ImageFormatR32f) ||
                        (field.image_format == "rgba8" && type.image.format != spv::ImageFormatRgba8);
                    if (is_wrong_format)
                    {
                        return cxx::unexpected("Image format mismatch: " + std::string(field.path));
                    }
                }
                ++binding;
            }
            if (resources.size() != active_bindings)
            {
                return cxx::unexpected(std::string("Shader has resources outside PassSchema"));
            }
            std::size_t actual_scalar_count = 0;
            for (const auto& list : {native.uniform_buffers, native.storage_buffers, native.push_constant_buffers})
            {
                for (const auto& block : list)
                {
                    actual_scalar_count += scalarCount(compiler, compiler.get_type(block.base_type_id));
                }
            }
            if (actual_scalar_count != static_cast<std::size_t>(std::count_if(
                                           contract.scalars.begin(),
                                           contract.scalars.end(),
                                           [stage_bit](const auto& field) { return (field.stages & stage_bit) != 0; }
                                       )))
            {
                return cxx::unexpected(std::string("Shader scalar field coverage mismatch"));
            }
            for (const auto& scalar : contract.scalars)
            {
                const auto model = compiler.get_execution_model();
                const auto stage =
                    model == spv::ExecutionModelGLCompute ? 4u : (model == spv::ExecutionModelVertex ? 1u : 2u);
                if ((scalar.stages & stage) == 0)
                {
                    continue;
                }
                std::optional<ScalarLocation> location;
                bool in_block = false;
                for (const auto& field : contract.resources)
                {
                    const auto prefix = std::string(field.path) + ".data";
                    if (!scalar.path.starts_with(prefix))
                    {
                        continue;
                    }
                    in_block = true;
                    for (const auto& resource : resources)
                    {
                        if (compiler.get_name(resource.id) == field.shader_name)
                        {
                            const auto& block = compiler.get_type(resource.base_type_id);
                            location = locate(compiler, block, scalar.path.substr(field.path.size() + 1));
                        }
                    }
                }
                if (!in_block && native.push_constant_buffers.size() == 1)
                {
                    std::string name = "p_";
                    for (const char c : scalar.path)
                    {
                        if (c == '.' || c == '[')
                        {
                            name += '_';
                        }
                        else if (c != ']')
                        {
                            name += c;
                        }
                    }
                    location = locate(compiler, compiler.get_type(native.push_constant_buffers[0].base_type_id), name);
                }
                if (!location || location->offset != scalar.offset)
                {
                    return cxx::unexpected("Scalar offset mismatch: " + std::string(scalar.path));
                }
                const auto& type = *location->type;
                const auto expected = scalar.kind == rdesc::EScalarKind::FLOAT ? sc::SPIRType::Float
                                      : scalar.kind == rdesc::EScalarKind::INT ? sc::SPIRType::Int
                                                                               : sc::SPIRType::UInt;
                if (type.basetype != expected || type.width != 32 || type.vecsize != 1 || type.columns != 1)
                {
                    return cxx::unexpected("Scalar type mismatch: " + std::string(scalar.path));
                }
            }
            return {};
        }
    } // namespace

    cxx::expected<void, std::string> validatePassSpirv(
        std::span<const std::uint32_t> words,
        const rdesc::PassShaderContract& contract
    ) noexcept
    {
        if (words.size() < 5 || words[0] != 0x07230203)
        {
            return cxx::unexpected(std::string("Invalid SPIR-V header"));
        }
        try
        {
            return validate(words, contract);
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (const std::exception& error)
        {
            return cxx::unexpected(std::string("SPIR-V compiler failure: ") + error.what());
        }
    }

    cxx::expected<void, std::string> validatePassShaders(
        std::span<const PassShaderModule> modules,
        const rdesc::PassShaderContract& contract,
        std::uint32_t required_stages
    ) noexcept
    {
        if (required_stages != 3u && required_stages != 4u)
        {
            return cxx::unexpected(std::string("Expected vertex+fragment or compute program"));
        }
        const bool uncovered_resource = std::any_of(
            contract.resources.begin(),
            contract.resources.end(),
            [required_stages](const auto& field)
            { return shaderResource(field.role) && (field.stages & required_stages) == 0; }
        );
        const bool uncovered_scalar = std::any_of(
            contract.scalars.begin(),
            contract.scalars.end(),
            [required_stages](const auto& field) { return (field.stages & required_stages) == 0; }
        );
        if (uncovered_resource || uncovered_scalar)
        {
            return cxx::unexpected(std::string("Pass field has no stage in the program"));
        }
        try
        {
            std::uint32_t observed = 0;
            for (const auto& module : modules)
            {
                const auto valid = validatePassSpirv(module.words, contract);
                if (!valid)
                {
                    return cxx::unexpected(valid.error());
                }
                sc::Compiler compiler(module.words.data(), module.words.size());
                const auto model = compiler.get_execution_model();
                const auto stage =
                    model == spv::ExecutionModelGLCompute ? 4u : (model == spv::ExecutionModelVertex ? 1u : 2u);
                if ((observed & stage) != 0 || (required_stages & stage) == 0)
                {
                    return cxx::unexpected(std::string("Duplicate or unexpected Shader stage"));
                }
                observed |= stage;
            }
            if (observed != required_stages)
            {
                return cxx::unexpected(std::string("Incomplete Pass Shader program"));
            }
            return {};
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (const std::exception& error)
        {
            return cxx::unexpected(std::string("SPIR-V compiler failure: ") + error.what());
        }
    }
} // namespace lux::toolchain
