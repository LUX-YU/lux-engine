#pragma once

#include <lux/engine/render/resource/Material.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Upload.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <lux/cxx/container/SlotMap.hpp>

namespace lux::render::vulkan
{
    struct NativeResourcePlan;
    struct PreparedStorageBinding;
    [[nodiscard]] StorageFactory materialStorageFactory() noexcept;

    class MaterialStorage final
    {
        struct Physical;
        struct Backing;
        struct Candidate;
        struct Use;
        struct Entry final { Backing* current = nullptr; bool updating = false; };

    public:
        MaterialStorage(DeviceSession&, StorageId, MaterialStorageConfig) noexcept;
        ~MaterialStorage() noexcept;
        MaterialStorage(const MaterialStorage&) = delete;
        MaterialStorage& operator=(const MaterialStorage&) = delete;
        [[nodiscard]] StorageId id() const noexcept { return id_; }
        [[nodiscard]] bool compatibleConfig(const MaterialStorageConfig& value) const noexcept { return value == config_; }
        [[nodiscard]] RenderResult<MaterialInformation> query(MaterialId) const noexcept;
        [[nodiscard]] RenderResult<void> release(MaterialId, std::uint64_t expected_revision) noexcept;
        [[nodiscard]] std::uint64_t residentBytes() const noexcept;
        [[nodiscard]] std::size_t residentVersions() const noexcept;

    private:
        friend StorageFactory materialStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<MaterialStorage>;
        using UploadResult = RenderResult<std::pair<MaterialPublished, GpuWork>>;
        [[nodiscard]] UploadResult upload(const MaterialWrite&) noexcept;
        [[nodiscard]] RenderResult<void> prepare(const NativeResourcePlan&, const MaterialBinding&,
            PreparedStorageBinding&) noexcept;
        void releaseReference(Backing&) noexcept;
        void retireUnused(Backing&) noexcept;
        void erase(Backing&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        MaterialStorageConfig config_;
        cxx::SlotMap<Entry, MaterialTag> entries_;
        std::vector<std::unique_ptr<Backing>> backings_;
        std::vector<Use*> uses_;
        std::shared_ptr<Physical> physical_;
    };
}

namespace lux::render
{
    template <> struct StorageTraits<vulkan::MaterialStorage> final
    {
        using Result = RenderResult<std::pair<MaterialPublished, vulkan::GpuWork>>;
        [[nodiscard]] static Result upload(vulkan::MaterialStorage&, const MaterialWrite&) noexcept;
    };
}
