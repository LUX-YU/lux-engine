#pragma once

#include <lux/engine/render/resource/Mesh.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Upload.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <lux/cxx/container/SlotMap.hpp>

namespace lux::render::vulkan
{
    struct NativeResourcePlan;
    struct PreparedStorageBinding;
    [[nodiscard]] StorageFactory meshStorageFactory() noexcept;

    class MeshStorage final
    {
        struct Physical;
        struct Backing;
        struct Candidate;
        struct Use;
        struct Entry final { Backing* current = nullptr; bool updating = false; };

    public:
        MeshStorage(DeviceSession&, StorageId, MeshStorageConfig) noexcept;
        ~MeshStorage() noexcept;
        MeshStorage(const MeshStorage&) = delete;
        MeshStorage& operator=(const MeshStorage&) = delete;
        [[nodiscard]] StorageId id() const noexcept { return id_; }
        [[nodiscard]] bool compatibleConfig(const MeshStorageConfig& value) const noexcept { return value == config_; }
        [[nodiscard]] RenderResult<MeshInformation> query(MeshId) const noexcept;
        [[nodiscard]] RenderResult<void> release(MeshId, std::uint64_t expected_revision) noexcept;
        [[nodiscard]] std::uint64_t residentBytes() const noexcept;
        [[nodiscard]] std::size_t residentVersions() const noexcept;

    private:
        friend StorageFactory meshStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<MeshStorage>;
        using UploadResult = RenderResult<std::pair<MeshPublished, GpuWork>>;
        [[nodiscard]] UploadResult upload(const MeshWrite&) noexcept;
        [[nodiscard]] RenderResult<void> validate(const MeshDescription&, std::span<const std::byte>) const noexcept;
        [[nodiscard]] RenderResult<void> prepare(const NativeResourcePlan&, const MeshBinding&,
            PreparedStorageBinding&) noexcept;
        void releaseReference(Backing&) noexcept;
        void retireUnused(Backing&) noexcept;
        void erase(Backing&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        MeshStorageConfig config_;
        cxx::SlotMap<Entry, MeshTag> entries_;
        std::vector<std::unique_ptr<Backing>> backings_;
        std::vector<Use*> uses_;
        std::shared_ptr<Physical> physical_;
    };
}

namespace lux::render
{
    template <> struct StorageTraits<vulkan::MeshStorage> final
    {
        using Result = RenderResult<std::pair<MeshPublished, vulkan::GpuWork>>;
        [[nodiscard]] static Result upload(vulkan::MeshStorage&, const MeshWrite&) noexcept;
    };
}
