#pragma once

#include <lux/engine/render/extensions/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/resources/mesh/Resource.hpp>

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/device/BufferArena.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

namespace lux::render::detail
{

    class IndexedTriangleMeshStorage final
    {
        struct Geometry final
        {
            ArenaAllocation vertices, indices, sections;
        };

        struct Entry final
        {
            MeshDescription description;
            Geometry geometry;
        };

    public:
        using UploadResult = exec::task<RenderResult<MeshId>>;
        [[nodiscard]] UploadResult upload(const rdesc::Mesh&) noexcept;

        using Descriptor = MeshResource;
        [[nodiscard]] static ResourceRegistration factory() noexcept;

        IndexedTriangleMeshStorage(ResourceCreateContext&, ResourceInstanceId) noexcept;
        ~IndexedTriangleMeshStorage() noexcept;
        IndexedTriangleMeshStorage(const IndexedTriangleMeshStorage&) = delete;
        IndexedTriangleMeshStorage& operator=(const IndexedTriangleMeshStorage&) = delete;

        [[nodiscard]] ResourceInstanceId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] const MeshDescription* find(MeshId) const noexcept;

        [[nodiscard]] RenderResult<IndexedGeometryInput> geometry(MeshId) const noexcept;
        [[nodiscard]] RenderResult<MeshArenaSection> arenaSection(MeshId, std::uint32_t submesh) const noexcept;
        [[nodiscard]] RenderResult<void> release(MeshId) noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;


    private:
        [[nodiscard]] RenderResult<void> validate(const MeshDescription&) const noexcept;
        [[nodiscard]] const Entry* findEntry(MeshId) const noexcept;
        [[nodiscard]] Entry* findEntry(MeshId) noexcept;
        void retire(const Geometry&) noexcept;
        DeviceSession& session_;
        ResourceUploader& uploads_;

        ResourceInstanceId id_;
        cxx::StableSlotMap<Entry, MeshTag> entries_;
        BufferArena vertices_, indices_, sections_;
    };
} // namespace lux::render::detail
