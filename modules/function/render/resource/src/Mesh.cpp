#include <lux/engine/render/resource/Mesh.hpp>
#include <lux/cxx/core/CheckedArithmetic.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<std::size_t> retained(const MeshDescription& description, std::size_t bytes) noexcept
        {
            const auto attributes = cxx::checkedMul(description.attributes.capacity(), sizeof(MeshAttribute));
            const auto submeshes = cxx::checkedMul(description.submeshes.capacity(), sizeof(MeshSubmesh));
            if (!attributes || !submeshes) return cxx::unexpected(RenderError{kProtocolContract});
            const auto first = cxx::checkedAdd(bytes, *attributes);
            if (!first) return cxx::unexpected(RenderError{kProtocolContract});
            const auto total = cxx::checkedAdd(*first, *submeshes);
            if (!total) return cxx::unexpected(RenderError{kProtocolContract});
            return *total;
        }
    }

    RenderResult<std::size_t> ConfigValueTraits<MeshBinding>::retainedBytes(const MeshBinding& input) noexcept
    {
        return retained(input.description, sizeof(input));
    }

    RenderResult<std::size_t> CommandValueTraits<MeshWrite>::retainedBytes(const MeshWrite& input) noexcept
    {
        const auto bytes = cxx::checkedAdd(sizeof(input), input.source.retainedBytes());
        if (!bytes) return cxx::unexpected(RenderError{kProtocolContract});
        return retained(input.description, *bytes);
    }

    RenderResult<std::size_t> CommandValueTraits<MeshInformation>::retainedBytes(const MeshInformation& input) noexcept
    {
        return retained(input.description, sizeof(input));
    }
}
