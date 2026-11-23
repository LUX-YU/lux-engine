#pragma once

#include <lux/engine/render/resource/mesh/Data.hpp>
#include <lux/engine/render/resource/mesh/Binding.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/BuiltinResources.hpp>
#include <lux/engine/render/graph/DrawResource.hpp>
#include <lux/engine/render/graph/DrawList.hpp>

namespace lux::render
{
    class CpuCallFrame;
    namespace detail { class IndexedTriangleMeshStorage; }

    class MeshResource final
    {
    public:
        using PerFrameData = std::tuple<>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kMeshResourceName,
            0,
            0,
            cxx::make_basic_type_info<MeshBinding>(),
            "Indexed triangle geometry with vertex and index arenas and section records.",
            EResourceDomain::DEVICE
        };


        using DrawStorage = detail::IndexedTriangleMeshStorage;
        [[nodiscard]] static LUX_RENDER_PUBLIC RenderResult<RasterIndexedGeometry>
        geometry(CpuCallFrame&, DrawResourceSlot<DrawStorage>, MeshId) noexcept;
        [[nodiscard]] static LUX_RENDER_PUBLIC DrawResourceType drawResourceType() noexcept;
    };
} // namespace lux::render
