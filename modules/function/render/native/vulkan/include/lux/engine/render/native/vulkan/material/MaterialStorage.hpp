#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/resource/Material.hpp>
#include <lux/engine/render/resource/Upload.hpp>
#include <map>

namespace lux::render::vulkan
{
    [[nodiscard]] StorageFactory materialStorageFactory() noexcept;

    class MaterialStorage final
    {
        struct Table;

        struct TextureLink final
        {
            TextureId texture;
            std::size_t position = 0;
        };

        struct Entry final
        {
            Table* table = nullptr;
            std::uint32_t slot = 0;
            std::vector<TextureLink> textures;
            StorageSubscription* bindings = nullptr;
            std::uint64_t revision = 0;
            bool updating = false;
        };

        using TextureKey = std::array<std::uint32_t, 4>;
        using StorageKey = std::pair<std::uint32_t, std::uint32_t>;

    public:
        MaterialStorage(DeviceSession&, StorageId) noexcept;
        ~MaterialStorage() noexcept;
        MaterialStorage(const MaterialStorage&) = delete;
        MaterialStorage& operator=(const MaterialStorage&) = delete;

        [[nodiscard]] StorageId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] RenderResult<MaterialInformation> query(MaterialId) const noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> gpuIndex(MaterialId) const noexcept;
        [[nodiscard]] bool sameTable(MaterialId, MaterialId) const noexcept;
        [[nodiscard]] RenderResult<void> release(MaterialId, std::uint64_t expected_revision) noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;

    private:
        friend StorageFactory materialStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<MaterialStorage>;
        using UploadResult = exec::task<RenderResult<MaterialPublished>>;
        [[nodiscard]] UploadResult upload(MaterialWrite) noexcept;
        [[nodiscard]] RenderResult<StorageResource>
        resolve(const NativeResourcePlan&, const MaterialBinding&, StorageSubscription&) noexcept;
        [[nodiscard]] RenderResult<Table*> table(MaterialProgram&) noexcept;
        [[nodiscard]] std::uint32_t textureIndex(TextureId) const noexcept;
        void watch(TextureId) noexcept;
        void link(MaterialId, Entry&, std::span<const TextureId>) noexcept;
        void unlink(Entry&) noexcept;
        void recordTexture(VkCommandBuffer, TextureId, std::uint32_t) noexcept;
        void adoptTexture(TextureId, std::uint32_t) noexcept;
        static TextureKey key(TextureId id) noexcept;
        static void publish(Table&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        cxx::StableSlotMap<Entry, MaterialTag> entries_;
        std::vector<std::unique_ptr<Table>> tables_;
        std::map<TextureKey, std::vector<std::pair<MaterialId, std::uint32_t>>> references_;
        std::map<StorageKey, TextureStorage::Observer> observers_;
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
