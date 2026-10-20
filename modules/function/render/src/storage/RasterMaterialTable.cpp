#include <lux/engine/render/storage/RasterMaterialTable.hpp>

namespace lux::render
{
    namespace
    {
        std::size_t stringBytes(const std::string& value) noexcept
        {
            const auto object = reinterpret_cast<std::uintptr_t>(&value);
            const auto data = reinterpret_cast<std::uintptr_t>(value.data());
            const bool is_inline = data >= object && data < object + sizeof(value);
            return is_inline || value.capacity() == 0 ? 0 : value.capacity() + 1;
        }

        std::size_t blockBytes(const rdesc::ShaderBlock& block) noexcept
        {
            auto bytes = block.fields.capacity() * sizeof(rdesc::ShaderMember);
            for (const auto& field : block.fields)
            {
                bytes += stringBytes(field.path);
            }
            return bytes;
        }
    } // namespace

    RenderResult<std::size_t> ConfigValueTraits<MaterialProgram>::retainedBytes(const MaterialProgram& value) noexcept
    {
        // These are actual, distinct C++ allocations, not externally supplied lengths or borrowed assets.
        const auto& info = value.interface;
        std::size_t bytes = sizeof(value) + value.fragment.words.capacity() * sizeof(std::uint32_t) +
                            stringBytes(value.fragment.entry) +
                            info.entry_points.capacity() * sizeof(rdesc::EntryPointInfo) +
                            info.sets.capacity() * sizeof(rdesc::DescriptorSetLayoutInfo) +
                            info.push_constants.capacity() * sizeof(rdesc::PushConstantRangeInfo) +
                            info.spec_constants.capacity() * sizeof(rdesc::SpecConstantInfo) +
                            info.vertex_inputs.capacity() * sizeof(rdesc::VertexInputAttribute) +
                            info.private_set_remap.capacity() * sizeof(std::pair<std::uint32_t, std::uint32_t>);
        for (const auto& entry : info.entry_points)
        {
            bytes += stringBytes(entry.name);
        }
        for (const auto& set : info.sets)
        {
            bytes += set.bindings.capacity() * sizeof(rdesc::EDescriptorBindingInfo);
            for (const auto& binding : set.bindings)
            {
                bytes += stringBytes(binding.name) + blockBytes(binding.block);
            }
        }
        for (const auto& range : info.push_constants)
        {
            bytes += blockBytes(range.block);
        }
        for (const auto& constant : info.spec_constants)
        {
            bytes += stringBytes(constant.name);
        }
        for (const auto& input : info.vertex_inputs)
        {
            bytes += stringBytes(input.name);
        }
        return bytes;
    }

    RenderResult<std::size_t> ConfigValueTraits<MaterialBinding>::retainedBytes(const MaterialBinding& value) noexcept
    {
        return sizeof(value) + blockBytes(value.layout);
    }
} // namespace lux::render
