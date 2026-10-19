#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Mesh.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace lux::render::vulkan
{
    class BufferArena;
    [[nodiscard]] StorageFactory meshStorageFactory() noexcept;

    class MeshStorage final
    {
        struct Geometry;

        struct Entry final
        {
            MeshDescription description;
            std::unique_ptr<Geometry> geometry;
            StorageSubscription* vertices = nullptr;
            StorageSubscription* indices = nullptr;
            StorageSubscription* sections = nullptr;
            std::uint64_t revision = 0;
            bool updating = false;
        };

    public:
        MeshStorage(DeviceSession&, StorageId) noexcept;
        ~MeshStorage() noexcept;
        MeshStorage(const MeshStorage&) = delete;
        MeshStorage& operator=(const MeshStorage&) = delete;

        [[nodiscard]] StorageId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] const MeshDescription* find(MeshId) const noexcept;
        [[nodiscard]] RenderResult<MeshInformation> query(MeshId) const noexcept;
        [[nodiscard]] RenderResult<void> release(MeshId, std::uint64_t expected_revision) noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;

        [[nodiscard]] RenderResult<StorageResource>
        resolve(const NativeResourcePlan&, const MeshBinding&, StorageSubscription&) noexcept;

    private:
        friend StorageFactory meshStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<MeshStorage>;
        using UploadResult = exec::task<RenderResult<MeshPublished>>;
        [[nodiscard]] UploadResult upload(MeshWrite) noexcept;
        [[nodiscard]] RenderResult<void> validate(const MeshDescription&, std::span<const std::byte>) const noexcept;
        void retire(std::unique_ptr<Geometry>&) noexcept;
        static void disconnect(Entry&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        cxx::StableSlotMap<Entry, MeshTag> entries_;
        std::unique_ptr<BufferArena> vertices_, indices_, sections_;
    };
} // namespace lux::render::vulkan

namespace lux::render
{
    template <> struct StorageTraits<vulkan::MeshStorage> final
    {
        using Result = exec::task<RenderResult<MeshPublished>>;
        [[nodiscard]] static Result upload(vulkan::MeshStorage&, MeshWrite) noexcept;
    };
} // namespace lux::render
