#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/features/ParameterLayout.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

namespace lux::render
{
    RenderResult<std::size_t>
    ConfigValueTraits<MeshFeatureConfig>::retainedBytes(const MeshFeatureConfig& value) noexcept
    {
        return sizeof(value) + detail::parameterLayoutBytes(value.input_layout);
    }
} // namespace lux::render
