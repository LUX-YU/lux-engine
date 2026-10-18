#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Material.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace lux::render::vulkan
{
    class BufferArena;
    struct NativeResourcePlan;
    struct PreparedStorageBinding;
    [[nodiscard]] StorageFactory materialStorageFactory() noexcept;

    class MaterialStorage final
    {
        struct Table;
        struct MaterialData;
        struct Candidate;
        struct Use;

        struct Entry final
        {
            std::unique_ptr<MaterialData> current;
            std::vector<Use*> uses;
            bool updating = false;
        };

    public:
        MaterialStorage(DeviceSession&, StorageId, MaterialStorageConfig) noexcept;
        ~MaterialStorage() noexcept;
        MaterialStorage(const MaterialStorage&) = delete;
        MaterialStorage& operator=(const MaterialStorage&) = delete;

        [[nodiscard]] StorageId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] bool compatibleConfig(const MaterialStorageConfig& value) const noexcept
        {
            return value == config_;
        }

        [[nodiscard]] RenderResult<MaterialInformation> query(MaterialId) const noexcept;
        [[nodiscard]] RenderResult<void> release(MaterialId, std::uint64_t expected_revision) noexcept;
        [[nodiscard]] std::uint64_t residentBytes() const noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;

    private:
        friend StorageFactory materialStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<MaterialStorage>;
        using UploadResult = exec::task<RenderResult<MaterialPublished>>;
        [[nodiscard]] UploadResult upload(MaterialWrite) noexcept;
        [[nodiscard]] RenderResult<void>
        prepare(const NativeResourcePlan&, const MaterialBinding&, PreparedStorageBinding&) noexcept;
        void retire(std::unique_ptr<MaterialData>&) noexcept;
        void refreshTextures(MaterialData&) noexcept;
        void unsubscribe(Use&) noexcept;
        static void disconnect(Entry&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        MaterialStorageConfig config_;
        cxx::SlotMap<Entry, MaterialTag> entries_;
        std::vector<std::unique_ptr<Table>> tables_;
    };
} // namespace lux::render::vulkan

namespace lux::render
{
    template <> struct StorageTraits<vulkan::MaterialStorage> final
    {
        using Result = exec::task<RenderResult<MaterialPublished>>;
        [[nodiscard]] static Result upload(vulkan::MaterialStorage&, MaterialWrite) noexcept;
    };
} // namespace lux::render
