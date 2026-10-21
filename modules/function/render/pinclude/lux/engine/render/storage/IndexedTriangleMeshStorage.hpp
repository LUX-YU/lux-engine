#pragma once

#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/StorageCreateContext.hpp>

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/memory/BufferArena.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/StorageFactory.hpp>
#include <lux/engine/render/resource/Upload.hpp>
#include <lux/engine/render/storage/IndexedTriangleMesh.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>

namespace lux::render::detail
{
    [[nodiscard]] StorageFactory meshStorageFactory() noexcept;

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
        resolve(const CompiledResource&, const MeshBinding&, StorageSubscription&) noexcept;

    private:
        friend StorageFactory meshStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<IndexedTriangleMeshStorage>;
        using UploadResult = exec::task<RenderResult<MeshId>>;
        [[nodiscard]] UploadResult upload(MeshWrite) noexcept;
        [[nodiscard]] RenderResult<void> validate(const MeshDescription&, std::span<const std::byte>) const noexcept;
        void retire(Geometry&) noexcept;
        DeviceSession& session_;
        ResourceUploader& uploads_;

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
