#pragma once

#include <lux/engine/render/resource/mesh/Data.hpp>
#include <lux/engine/render/resource/mesh/Binding.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/graph/DrawResource.hpp>
#include <lux/engine/render/graph/DrawList.hpp>

namespace lux::render
{
    inline constexpr ResourceNameView kMeshResourceName{"lux.render.resource.mesh"};

    namespace detail { class IndexedTriangleMeshStorage; struct CpuDrawStorage; }

    // Borrowed by an installed workspace for its synchronous draw selections.
    class LUX_RENDER_PUBLIC MeshDrawBinding final
    {
    public:
        [[nodiscard]] static RenderResult<MeshDrawBinding> bind(const InstalledDrawSources&, RasterResourceSlot) noexcept;
        [[nodiscard]] RenderResult<RasterIndexedGeometry> geometry(MeshId) const noexcept;

    private:
        MeshDrawBinding(const detail::IndexedTriangleMeshStorage& storage, detail::CpuDrawStorage& output) noexcept
            : storage_(storage), output_(output) {}

        const detail::IndexedTriangleMeshStorage& storage_;
        detail::CpuDrawStorage& output_;
    };

    class MeshResource final
    {
    public:
        using PerFrameData = std::tuple<>;
        using Config = void;
        static inline const ResourceDefinition descriptor{
            kMeshResourceName,
            {},
            {},
            cxx::make_basic_type_info<MeshBinding>(),
            "Indexed triangle geometry with vertex and index arenas and section records.",
            EResourceDomain::DEVICE
        };
    };
} // namespace lux::render
