#include <lux/engine/render/resource/Material.hpp>
#include <lux/cxx/core/CheckedArithmetic.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<std::size_t> add(std::size_t a, std::size_t b) noexcept
        {
            const auto result = cxx::checkedAdd(a, b);
            if (!result) return cxx::unexpected(RenderError{kProtocolContract});
            return *result;
        }
    }

    const ShaderBlock& materialParameterBlock() noexcept
    {
        static const ShaderBlock value = [] {
            ShaderBlock block{416, 16, rdesc::EDataLayout::STD430, {}, true};
            const std::array names{"shading_model_id", "feature_mask", "tex_mask", "flags"};
            for (unsigned i = 0; i < names.size(); ++i)
                block.fields.push_back({names[i], rdesc::EDataScalar::UINT, i * 4, 1, 1, 0, 0});
            for (unsigned i = 0; i < 4; ++i)
                block.fields.push_back({"_header_pad[" + std::to_string(i) + "]",
                    rdesc::EDataScalar::UINT, 16 + i * 4, 1, 1, 4, 0});
            for (unsigned i = 0; i < 16; ++i)
                block.fields.push_back({"params[" + std::to_string(i) + "]",
                    rdesc::EDataScalar::FLOAT, 32 + i * 16, 4, 1, 16, 0});
            const std::array texture_fields{"representation_index", "resource_index", "aux", "flags"};
            for (unsigned i = 0; i < 8; ++i)
                for (unsigned j = 0; j < texture_fields.size(); ++j)
                    block.fields.push_back({"tex[" + std::to_string(i) + "]." + texture_fields[j],
                        rdesc::EDataScalar::UINT, 288 + i * 16 + j * 4, 1, 1, 16, 0});
            return block;
        }();
        return value;
    }

    RenderResult<std::size_t> ConfigValueTraits<MaterialProgram>::retainedBytes(const MaterialProgram& value) noexcept
    {
        const auto words = cxx::checkedMul(value.fragment.words.capacity(), sizeof(std::uint32_t));
        if (!words) return cxx::unexpected(RenderError{kProtocolContract});
        auto bytes = add(sizeof(value), *words);
        if (!bytes) return bytes;
        return add(*bytes, value.fragment.entry.capacity() + 1);
    }

    RenderResult<std::size_t> ConfigValueTraits<MaterialBinding>::retainedBytes(const MaterialBinding& value) noexcept
    {
        return add(sizeof(value), value.program.retainedBytes());
    }




}
