#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/extensions/storage/IndexedTriangleMesh.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<std::size_t> retained(const MeshDescription& description, std::size_t bytes) noexcept
        {
            const auto attributes = cxx::checkedMul(description.attributes.capacity(), sizeof(MeshAttribute));
            const auto submeshes = cxx::checkedMul(description.submeshes.capacity(), sizeof(MeshSubmesh));
            if (!attributes || !submeshes)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            const auto first = cxx::checkedAdd(bytes, *attributes);
            if (!first)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            const auto total = cxx::checkedAdd(*first, *submeshes);
            if (!total)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            return *total;
        }
    } // namespace

    RenderResult<std::size_t> ConfigValueTraits<MeshDescription>::retainedBytes(const MeshDescription& input) noexcept
    {
        return retained(input, sizeof(input));
    }

    RenderResult<std::size_t> ConfigValueTraits<MeshBinding>::retainedBytes(const MeshBinding& input) noexcept
    {
        return sizeof(input) + input.attributes.capacity() * sizeof(MeshAttribute);
    }

} // namespace lux::render
