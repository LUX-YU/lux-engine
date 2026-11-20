#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <concepts>
#include <type_traits>

namespace lux::render
{
    struct ConstantMemberLayout final
    {
        std::string_view path;
        std::uint32_t offset;
        rdesc::EDataScalar scalar;
        std::uint32_t rows = 1, columns = 1, matrix_stride = 0;
        rdesc::EShaderMatrixOrder matrix_order = rdesc::EShaderMatrixOrder::NOT_MATRIX;
        std::span<const rdesc::ShaderArrayDimension> array_dimensions;
    };

    struct ConstantBlockLayout final
    {
        std::uint32_t bytes, alignment;
        std::span<const ConstantMemberLayout> members;
    };

    // Describe the target ABI of a complete encoding, including any explicit padding.
    template <class T> struct ConstantBlockTraits;

    template <class T>
    concept ShaderConstantBlock = std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T> &&
        requires(rdesc::EShaderTarget target)
        {
            { ConstantBlockTraits<T>::layout(target) } noexcept -> std::same_as<ConstantBlockLayout>;
        };

    struct ConstantBlockContract final
    {
        cxx::basic_type_info type;
        ConstantBlockLayout (*layout)(rdesc::EShaderTarget) noexcept = nullptr;
        std::uint32_t bytes = 0;
        std::uint32_t alignment = 0;
    };

    template <ShaderConstantBlock T>
    inline constexpr ConstantBlockContract kConstantBlockContract{
        cxx::make_basic_type_info<T>(), &ConstantBlockTraits<T>::layout, sizeof(T), alignof(T)
    };
} // namespace lux::render
