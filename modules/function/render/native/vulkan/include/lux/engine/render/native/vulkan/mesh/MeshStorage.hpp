#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Mesh.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace lux::render::vulkan
{
    class BufferArena;
    struct NativeResourcePlan;
    struct PreparedStorageBinding;
    [[nodiscard]] StorageFactory meshStorageFactory() noexcept;

    class MeshStorage final
    {
        struct MeshData;
        struct Candidate;
        struct Use;

        struct Entry final
        {
            std::unique_ptr<MeshData> current;
            std::vector<Use*> uses;
            bool updating = false;
        };

    public:
        MeshStorage(DeviceSession&, StorageId, MeshStorageConfig) noexcept;
        ~MeshStorage() noexcept;
        MeshStorage(const MeshStorage&) = delete;
        MeshStorage& operator=(const MeshStorage&) = delete;

        [[nodiscard]] StorageId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] bool compatibleConfig(const MeshStorageConfig& value) const noexcept
        {
            return value == config_;
        }

        [[nodiscard]] RenderResult<MeshInformation> query(MeshId) const noexcept;
        [[nodiscard]] RenderResult<void> release(MeshId, std::uint64_t expected_revision) noexcept;
        [[nodiscard]] std::uint64_t residentBytes() const noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;

    private:
        friend StorageFactory meshStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<MeshStorage>;
        using UploadResult = exec::task<RenderResult<MeshPublished>>;
        [[nodiscard]] UploadResult upload(MeshWrite) noexcept;
        [[nodiscard]] RenderResult<void> validate(const MeshDescription&, std::span<const std::byte>) const noexcept;
        [[nodiscard]] RenderResult<void>
        prepare(const NativeResourcePlan&, const MeshBinding&, PreparedStorageBinding&) noexcept;
        void retire(std::unique_ptr<MeshData>&) noexcept;
        void unsubscribe(Use&) noexcept;
        static void disconnect(Entry&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        MeshStorageConfig config_;
        cxx::SlotMap<Entry, MeshTag> entries_;
        std::unique_ptr<BufferArena> arena_;
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
