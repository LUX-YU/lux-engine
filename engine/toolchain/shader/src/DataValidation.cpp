#include <algorithm>
#include <bit>
#include <lux/engine/toolchain/shader/DataValidation.hpp>
#include <new>
#include <spirv_cross.hpp>
#include <vector>

namespace lux::toolchain
{
    namespace
    {
        namespace sc = SPIRV_CROSS_NAMESPACE;

        bool fields(
            const sc::Compiler& compiler,
            const sc::SPIRType& type,
            std::string path,
            std::uint32_t offset,
            std::uint32_t inherited_stride,
            std::vector<std::pair<std::string, rdesc::DataGpuField>>& result
        )
        {
            for (std::uint32_t i = 0; i < type.member_types.size(); ++i)
            {
                const auto& member = compiler.get_type(type.member_types[i]);
                const auto name = path + compiler.get_member_name(type.self, i);
                const auto base = offset + compiler.type_struct_member_offset(type, i);
                std::uint32_t count = 1, stride = inherited_stride;
                if (!member.array.empty())
                {
                    if (member.array.size() != 1 || !member.array_size_literal[0] || member.array[0] == 0)
                    {
                        return false;
                    }
                    count = member.array[0];
                    stride = compiler.type_struct_member_array_stride(type, i);
                }
                for (std::uint32_t index = 0; index < count; ++index)
                {
                    const auto item = name + (member.array.empty() ? "" : "[" + std::to_string(index) + "]");
                    const auto at = base + (member.array.empty() ? 0 : index * stride);
                    if (member.basetype == sc::SPIRType::Struct)
                    {
                        if (!fields(compiler, member, item + ".", at, stride, result))
                        {
                            return false;
                        }
                        continue;
                    }
                    const bool is_numeric = member.basetype == sc::SPIRType::Float ||
                                            member.basetype == sc::SPIRType::Int ||
                                            member.basetype == sc::SPIRType::UInt;
                    if (!is_numeric || member.width != 32 ||
                        compiler.has_member_decoration(type.self, i, spv::DecorationRowMajor))
                    {
                        return false;
                    }
                    const auto scalar = member.basetype == sc::SPIRType::Float ? rdesc::EDataScalar::FLOAT
                                        : member.basetype == sc::SPIRType::Int ? rdesc::EDataScalar::SINT
                                                                               : rdesc::EDataScalar::UINT;
                    const auto matrix_stride =
                        member.columns > 1 ? compiler.type_struct_member_matrix_stride(type, i) : 0;
                    result.push_back({item, {{}, scalar, at, member.vecsize, member.columns, stride, matrix_stride}});
                }
            }
            return true;
        }
    } // namespace

    cxx::expected<void, std::string> validateDataSpirv(
        std::span<const std::uint32_t> words,
        const rdesc::DataGpuLayout& layout,
        std::string_view resource_name
    ) noexcept
    {
        const bool is_invalid_header = words.size() < 5 || words[0] != 0x07230203;
        const bool is_invalid_layout =
            !std::has_single_bit(layout.alignment) || layout.size == 0 || layout.fields.empty() ||
            (layout.storage != rdesc::EDataLayout::STD140 && layout.storage != rdesc::EDataLayout::STD430);
        if (is_invalid_header || is_invalid_layout)
        {
            return cxx::unexpected(std::string("Invalid Data layout or SPIR-V header"));
        }
        try
        {
            sc::Compiler compiler(words.data(), words.size());
            auto resources = compiler.get_shader_resources();
            const auto& list =
                layout.storage == rdesc::EDataLayout::STD140 ? resources.uniform_buffers : resources.storage_buffers;
            for (const auto& resource : list)
            {
                if (compiler.get_name(resource.id) != resource_name)
                {
                    continue;
                }
                const auto& block = compiler.get_type(resource.base_type_id);
                if (block.member_types.size() != 1 || compiler.type_struct_member_offset(block, 0) != 0)
                {
                    return cxx::unexpected(std::string("Data block must contain one record at offset zero"));
                }
                const auto& record = compiler.get_type(block.member_types[0]);
                if (record.basetype != sc::SPIRType::Struct || !record.array.empty())
                {
                    return cxx::unexpected(std::string("Data block record shape mismatch"));
                }
                std::vector<std::pair<std::string, rdesc::DataGpuField>> actual;
                if (!fields(compiler, record, "", 0, 0, actual))
                {
                    return cxx::unexpected(std::string("Unsupported reflected array/scalar/matrix"));
                }
                const auto size = compiler.get_declared_struct_size(record);
                const auto rounded =
                    layout.alignment ? (size + layout.alignment - 1) / layout.alignment * layout.alignment : 0;
                if (rounded != layout.size || actual.size() != layout.fields.size())
                {
                    return cxx::unexpected(std::string("Data size/field count mismatch"));
                }
                for (std::size_t i = 0; i < actual.size(); ++i)
                {
                    actual[i].second.path = actual[i].first;
                    if (actual[i].second != layout.fields[i])
                    {
                        return cxx::unexpected(std::string("Data ABI mismatch at ") + actual[i].first);
                    }
                }
                return {};
            }
            return cxx::unexpected(std::string("Data resource absent"));
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (const std::exception& error)
        {
            return cxx::unexpected(std::string("Data reflection failure: ") + error.what());
        }
    }
} // namespace lux::toolchain
