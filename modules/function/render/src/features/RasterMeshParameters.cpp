#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

namespace lux::render
{
    RenderResult<std::size_t> ConfigValueTraits<MeshFeatureConfig>::retainedBytes(const MeshFeatureConfig& value
    ) noexcept
    {
        auto mesh = ConfigValueTraits<MeshDescription>::retainedBytes(value.geometry);
        if (!mesh)
        {
            return cxx::unexpected(mesh.error());
        }
        const auto textures = cxx::checkedMul(value.textures.capacity(), sizeof(GraphImageDesc));
        if (!textures)
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        auto program = ConfigValueTraits<MaterialProgram>::retainedBytes(value.program);
        if (!program)
        {
            return cxx::unexpected(program.error());
        }
        std::size_t total = sizeof(value);
        for (auto bytes :
             {*mesh - sizeof(MeshDescription),
              *program - sizeof(MaterialProgram),
              *textures,
              value.mesh_storage.capacity(),
              value.material_storage.capacity(),
              value.instance_storage.capacity(),
              std::size_t(3)})
        {
            auto sum = cxx::checkedAdd(total, bytes);
            if (!sum)
            {
                return cxx::unexpected(RenderError{kInvalidConfig});
            }
            total = *sum;
        }
        return total;
    }
} // namespace lux::render
