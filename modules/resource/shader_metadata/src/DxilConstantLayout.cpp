#include "DxilConstantLayout.hpp"

#define NOMINMAX
#include <windows.h>
#include <d3d12shader.h>
#include <dxcapi.h>
#undef interface

#include <lux/cxx/core/scope_exit.hpp>
#include <slang-com-ptr.h>

namespace lux::shader::detail
{
    namespace
    {
        struct NativeMember final
        {
            rdesc::EShaderMatrixOrder order = rdesc::EShaderMatrixOrder::NOT_MATRIX;
            rdesc::EDataScalar scalar = rdesc::EDataScalar::FLOAT;
            std::uint32_t rows = 0, columns = 0, offset = 0, matrix_stride = 0;
            std::uint32_t elements = 0, stride = 0, extent = 0;
            std::vector<NativeMember> members;
        };

        bool reflectType(
            ID3D12ShaderReflectionType& type, NativeMember& result, std::uint32_t block_bytes, unsigned depth
        )
        {
            D3D12_SHADER_TYPE_DESC info{};
            if (depth > 32 || FAILED(type.GetDesc(&info))) return false;
            result.offset = info.Offset;
            std::uint64_t extent = 0;
            if (info.Class == D3D_SVC_STRUCT)
            {
                if (!info.Members || info.Members > block_bytes / 4) return false;
                result.members.resize(info.Members);
                for (unsigned index = 0; index < info.Members; ++index)
                {
                    auto* child = type.GetMemberTypeByIndex(index);
                    auto& member = result.members[index];
                    if (!child || !reflectType(*child, member, block_bytes, depth + 1)) return false;
                    extent = std::max(extent, std::uint64_t(member.offset) + member.extent);
                }
            }
            else
            {
                switch (info.Type)
                {
                case D3D_SVT_FLOAT: result.scalar = rdesc::EDataScalar::FLOAT; break;
                case D3D_SVT_INT: result.scalar = rdesc::EDataScalar::SINT; break;
                case D3D_SVT_UINT: result.scalar = rdesc::EDataScalar::UINT; break;
                default: return false;
                }
                if (info.Rows == 0 || info.Rows > 4 || info.Columns == 0 || info.Columns > 4) return false;
                switch (info.Class)
                {
                case D3D_SVC_SCALAR:
                case D3D_SVC_VECTOR:
                    result.rows = info.Columns;
                    result.columns = 1;
                    extent = 4u * info.Columns;
                    break;
                case D3D_SVC_MATRIX_ROWS:
                case D3D_SVC_MATRIX_COLUMNS:
                {
                    const bool row_major = info.Class == D3D_SVC_MATRIX_ROWS;
                    result.order = row_major ? rdesc::EShaderMatrixOrder::ROW_MAJOR
                                             : rdesc::EShaderMatrixOrder::COLUMN_MAJOR;
                    result.rows = info.Rows;
                    result.columns = info.Columns;
                    result.matrix_stride = 16;
                    const auto major = row_major ? info.Rows : info.Columns;
                    const auto minor = row_major ? info.Columns : info.Rows;
                    extent = (major - 1) * result.matrix_stride + 4u * minor;
                    break;
                }
                default: return false;
                }
            }
            if (extent > block_bytes) return false;
            if (info.Elements)
            {
                // DXIL constant-buffer packing places array elements at separate 16-byte registers.
                const auto stride = (extent + 15u) & ~std::uint64_t{15};
                result.elements = info.Elements;
                result.stride = static_cast<std::uint32_t>(stride);
                extent += std::uint64_t(info.Elements - 1) * stride;
                if (extent > block_bytes) return false;
            }
            result.extent = static_cast<std::uint32_t>(extent);
            return true;
        }

        bool sameArrayLayout(
            std::span<const rdesc::ShaderArrayDimension> declared,
            std::span<const rdesc::ShaderArrayDimension> native
        ) noexcept
        {
            // DXC reports direct multidimensional arrays as one physical array. Join adjacent
            // dimensions only when the entire inner array occupies exactly one outer stride.
            const auto take_group = [](auto& dimensions) noexcept
            {
                const auto stride = dimensions.back().stride;
                std::uint64_t count = dimensions.back().count;
                dimensions = dimensions.first(dimensions.size() - 1);
                while (!dimensions.empty() && count <= UINT32_MAX && count * stride == dimensions.back().stride)
                {
                    count *= dimensions.back().count;
                    dimensions = dimensions.first(dimensions.size() - 1);
                }
                return std::pair{count, stride};
            };
            while (!declared.empty() && !native.empty())
            {
                if (take_group(declared) != take_group(native)) return false;
            }
            return declared.empty() && native.empty();
        }

        bool compareMembers(
            const NativeMember& native,
            std::uint32_t base,
            const rdesc::ShaderBlock& block,
            std::vector<rdesc::ShaderArrayDimension>& arrays,
            std::size_t& cursor
        )
        {
            const bool out_of_range = base > block.size || native.offset > block.size - base;
            if (out_of_range) return false;
            base += native.offset;
            if (native.extent > block.size - base) return false;
            if (native.elements) arrays.push_back({native.elements, native.stride});
            for (unsigned item = 0; item < std::max(native.elements, 1u); ++item)
            {
                const auto offset = base + item * native.stride;
                if (!native.members.empty())
                {
                    for (const auto& member : native.members)
                        if (!compareMembers(member, offset, block, arrays, cursor)) return false;
                    continue;
                }
                if (cursor == block.fields.size()) return false;
                const auto& field = block.fields[cursor++];
                const bool same_type = field.scalar == native.scalar && field.rows == native.rows &&
                    field.columns == native.columns && field.matrix_order == native.order;
                const bool same_placement = field.offset == offset && field.matrix_stride == native.matrix_stride &&
                    sameArrayLayout(field.array_dimensions, arrays);
                if (!same_type || !same_placement) return false;
            }
            if (native.elements) arrays.pop_back();
            return true;
        }

        bool matchesBuffer(ID3D12ShaderReflectionConstantBuffer& native, const rdesc::ShaderBlock& block)
        {
            D3D12_SHADER_BUFFER_DESC description{};
            if (FAILED(native.GetDesc(&description)) || !block.hasConstantLayoutEvidence()) return false;
            // DXC can include the unused tail of the last register in the reflected buffer size.
            const auto align_register = [](std::uint64_t bytes) { return (bytes + 15u) & ~std::uint64_t{15}; };
            if (align_register(description.Size) != align_register(block.size)) return false;
            std::vector<rdesc::ShaderArrayDimension> arrays;
            std::size_t cursor = 0;
            for (unsigned index = 0; index < description.Variables; ++index)
            {
                auto* variable = native.GetVariableByIndex(index);
                D3D12_SHADER_VARIABLE_DESC info{};
                if (!variable || FAILED(variable->GetDesc(&info))) return false;
                auto* type = variable->GetType();
                NativeMember member;
                if (!type || !reflectType(*type, member, block.size, 0)) return false;
                member.offset = 0;
                if (!compareMembers(member, info.StartOffset, block, arrays, cursor)) return false;
            }
            return cursor == block.fields.size();
        }
    } // namespace

    EDxilConstantLayout
    verifyDxilConstants(const std::filesystem::path& path, const rdesc::ShaderProgramArtifact& shader) noexcept
    {
        using EResult = EDxilConstantLayout;
        const auto module = LoadLibraryExW(
            path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
        );
        if (!module) return EResult::REFLECTION_UNAVAILABLE;
        cxx::scope_exit unload([module]() noexcept { FreeLibrary(module); });
        const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
        Slang::ComPtr<IDxcUtils> utils;
        if (!create || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(utils.writeRef()))))
            return EResult::REFLECTION_UNAVAILABLE;
        const auto words = std::span(shader.stages.front().words);
        const DxcBuffer payload{words.data(), words.size_bytes(), 0};
        Slang::ComPtr<ID3D12ShaderReflection> reflection;
        if (FAILED(utils->CreateReflection(&payload, IID_PPV_ARGS(reflection.writeRef()))))
            return EResult::REFLECTION_UNAVAILABLE;
        D3D12_SHADER_DESC description{};
        if (FAILED(reflection->GetDesc(&description))) return EResult::REFLECTION_UNAVAILABLE;
        for (unsigned index = 0; index < description.BoundResources; ++index)
        {
            D3D12_SHADER_INPUT_BIND_DESC binding{};
            if (FAILED(reflection->GetResourceBindingDesc(index, &binding))) return EResult::REFLECTION_UNAVAILABLE;
            if (binding.Type != D3D_SIT_CBUFFER) continue;

            const rdesc::ShaderBlock* expected = nullptr;
            if (binding.Space == 255 && binding.BindPoint == 0)
            {
                const auto& constants = shader.interface.push_constants;
                if (binding.BindCount == 1 && constants.size() == 1 &&
                    constants.front().offset == 0 && constants.front().size == constants.front().block.size)
                    expected = &constants.front().block;
            }
            else
            {
                const auto set =
                    std::ranges::find(shader.interface.sets, binding.Space, &rdesc::DescriptorSetLayoutInfo::set);
                if (set != shader.interface.sets.end())
                {
                    const auto field =
                        std::ranges::find(set->bindings, binding.BindPoint, &rdesc::EDescriptorBindingInfo::binding);
                    if (field != set->bindings.end() && field->type == rdesc::EDescriptorType::UNIFORM_BUFFER &&
                        field->count == binding.BindCount)
                        expected = &field->block;
                }
            }
            auto* buffer = reflection->GetConstantBufferByName(binding.Name);
            if (!expected || !buffer || !matchesBuffer(*buffer, *expected)) return EResult::LAYOUT_MISMATCH;
        }
        return EResult::VERIFIED;
    }
} // namespace lux::shader::detail
