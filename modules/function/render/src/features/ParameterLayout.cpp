#include <lux/engine/render/features/ParameterLayout.hpp>

namespace lux::render::detail
{
    std::size_t parameterLayoutBytes(const rdesc::ShaderBlock& layout) noexcept
    {
        auto bytes = layout.fields.capacity() * sizeof(rdesc::ShaderMember);
        for (const auto& field : layout.fields)
        {
            const auto begin = reinterpret_cast<std::uintptr_t>(&field.path);
            const auto data = reinterpret_cast<std::uintptr_t>(field.path.data());
            if (data < begin || data >= begin + sizeof(field.path))
            {
                bytes += field.path.capacity() + 1;
            }
        }
        return bytes;
    }
} // namespace lux::render::detail
