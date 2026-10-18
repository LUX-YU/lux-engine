#include <algorithm>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/resource/Material.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<std::size_t> add(std::size_t a, std::size_t b) noexcept
        {
            const auto result = cxx::checkedAdd(a, b);
            if (!result)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            return *result;
        }
    } // namespace

    ShaderBlock materialParameterBlock(const MaterialProgram& program) noexcept
    {
        const auto parameter_count = std::max(1u, program.parameterCount());
        const auto texture_count = std::max(1u, program.texture_count);
        {
            ShaderBlock block{16 * parameter_count + 16 * texture_count, 16, rdesc::EDataLayout::STD430, {}, true};
            for (unsigned i = 0; i < parameter_count; ++i)
            {
                block.fields.push_back(
                    {"params[" + std::to_string(i) + "]", rdesc::EDataScalar::FLOAT, i * 16, 4, 1, 16, 0}
                );
            }
            const std::array texture_fields{"representation_index", "resource_index", "aux", "flags"};
            for (unsigned i = 0; i < texture_count; ++i)
            {
                for (unsigned j = 0; j < texture_fields.size(); ++j)
                {
                    block.fields.push_back(
                        {"tex[" + std::to_string(i) + "]." + texture_fields[j],
                         rdesc::EDataScalar::UINT,
                         parameter_count * 16 + i * 16 + j * 4,
                         1,
                         1,
                         16,
                         0}
                    );
                }
            }
            return block;
        }
    }

    RenderResult<std::size_t> ConfigValueTraits<MaterialProgram>::retainedBytes(const MaterialProgram& value) noexcept
    {
        const auto lanes =
            std::uint64_t(std::max<std::size_t>(1, value.defaults.size())) + std::max(1u, value.texture_count);
        if (lanes > UINT32_MAX / 16u)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const auto words = cxx::checkedMul(value.fragment.words.capacity(), sizeof(std::uint32_t));
        if (!words)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        auto bytes = add(sizeof(value), *words);
        if (!bytes)
        {
            return bytes;
        }
        bytes = add(*bytes, value.fragment.entry.capacity() + 1);
        const auto parameters = cxx::checkedMul(value.defaults.capacity(), sizeof(std::array<float, 4>));
        if (!bytes || !parameters)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        return add(*bytes, *parameters);
    }

    RenderResult<std::size_t> ConfigValueTraits<MaterialBinding>::retainedBytes(const MaterialBinding& value) noexcept
    {
        return add(sizeof(value), value.program.retainedBytes());
    }

} // namespace lux::render
