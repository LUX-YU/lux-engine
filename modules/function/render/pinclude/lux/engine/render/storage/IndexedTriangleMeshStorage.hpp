#pragma once

#include <lux/engine/render/extensions/execution/FrameExecution.hpp>
#include <lux/engine/render/extensions/storage/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/storage/StorageCreateContext.hpp>

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/extensions/storage/IndexedTriangleMesh.hpp>
#include <lux/engine/render/extensions/storage/StorageBinding.hpp>
#include <lux/engine/render/extensions/storage/StorageFactory.hpp>
#include <lux/engine/render/extensions/storage/Upload.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/memory/BufferArena.hpp>

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
        using Descriptor = IndexedTriangleMeshStorageDescriptor;
        [[nodiscard]] static StorageFactory factory() noexcept;

        IndexedTriangleMeshStorage(StorageCreateContext&, StorageId) noexcept;
        ~IndexedTriangleMeshStorage() noexcept;
        IndexedTriangleMeshStorage(const IndexedTriangleMeshStorage&) = delete;
        IndexedTriangleMeshStorage& operator=(const IndexedTriangleMeshStorage&) = delete;

        [[nodiscard]] StorageId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] const MeshDescription* find(MeshId) const noexcept;
        [[nodiscard]] RenderResult<void> release(MeshId) noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;

        [[nodiscard]] RenderResult<ResourceInput>
        resolve(const VGraphResourceDesc&, const MeshBinding&, StorageSubscription&) noexcept;

    private:
        friend struct lux::render::StorageTraits<IndexedTriangleMeshStorage>;
        using UploadResult = exec::task<RenderResult<MeshId>>;
        [[nodiscard]] UploadResult upload(MeshWrite) noexcept;
        [[nodiscard]] RenderResult<void> validate(const MeshDescription&, std::span<const std::byte>) const noexcept;
        void retire(Geometry&) noexcept;
        DeviceSession& session_;
        ResourceUploader& uploads_;
        FrameExecution& frames_;

        StorageId id_;
        cxx::StableSlotMap<Entry, MeshTag> entries_;
        BufferArena vertices_, indices_, sections_;
    };
} // namespace lux::render::detail

namespace lux::render
{
    template <> struct StorageTraits<detail::IndexedTriangleMeshStorage> final
    {
        using Result = exec::task<RenderResult<MeshId>>;
        [[nodiscard]] static Result upload(detail::IndexedTriangleMeshStorage&, MeshWrite) noexcept;
    };
} // namespace lux::render
