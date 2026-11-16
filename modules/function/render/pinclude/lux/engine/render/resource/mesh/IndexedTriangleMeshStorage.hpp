#pragma once

#include <lux/engine/render/extensions/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/resources/MeshResource.hpp>

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/device/BufferArena.hpp>
#include <lux/engine/render/extensions/resource/IndexedTriangleMesh.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

namespace lux::render::detail
{

    class IndexedTriangleMeshStorage final
    {
        struct Geometry final
        {
            BufferArena::Range vertices, indices, sections;
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

        struct BufferViews final
        {
            ResourceInput vertices, indices;
            std::uint32_t vertex_stride = 0, index_stride = 0;
        };

        [[nodiscard]] RenderResult<BufferViews> bufferViews(MeshId) const noexcept;
        [[nodiscard]] RenderResult<void> release(MeshId) noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;

        [[nodiscard]] RenderResult<ResourceInput>
        resolve(const VGraphResourceDesc&, const MeshBinding&, StorageSubscription&) noexcept;

    private:
        [[nodiscard]] RenderResult<void> validate(const MeshDescription&) const noexcept;
        [[nodiscard]] const Entry* findEntry(MeshId) const noexcept;
        [[nodiscard]] Entry* findEntry(MeshId) noexcept;
        void retire(Geometry&) noexcept;
        DeviceSession& session_;
        ResourceUploader& uploads_;

        ResourceInstanceId id_;
        cxx::StableSlotMap<Entry, MeshTag> entries_;
        BufferArena vertices_, indices_, sections_;
    };
} // namespace lux::render::detail
