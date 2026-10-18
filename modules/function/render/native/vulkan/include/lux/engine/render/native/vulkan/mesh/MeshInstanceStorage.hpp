#pragma once

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/MeshInstance.hpp>
#include <map>

namespace lux::render::vulkan
{
    class MeshDraws;
    struct NativeResourcePlan;
    struct PreparedStorageBinding;
    [[nodiscard]] StorageFactory meshInstanceStorageFactory() noexcept;

    // Mesh instance CPU authority. Only changed pages are recorded; accepted graphics submissions
    // order writes after prior draws. No mapped shadow banks or per-resource completion tokens.
    class MeshInstanceStorage final
    {
        struct Backing;
        struct Use;

        struct Entry final
        {
            MeshId mesh;
            MaterialId material;
            std::uint32_t submesh = 0;
            std::uint64_t revision = 1;
            cxx::SlotKey<MeshInstanceTag> key;
        };

        static constexpr std::uint32_t kPageSize = 512;

        struct Page final
        {
            std::uint32_t index;
            std::array<TransformMatrix, kPageSize> values;
        };

    public:
        MeshInstanceStorage(DeviceSession&, StorageId, MeshInstanceStorageConfig) noexcept;
        ~MeshInstanceStorage() noexcept;
        MeshInstanceStorage(const MeshInstanceStorage&) = delete;
        MeshInstanceStorage& operator=(const MeshInstanceStorage&) = delete;

        [[nodiscard]] bool compatibleConfig(const MeshInstanceStorageConfig& value) const noexcept
        {
            return value == config_;
        }

        [[nodiscard]] RenderResult<MeshInstancePublished> create(const MeshInstanceDescription&) noexcept;
        [[nodiscard]] RenderResult<TransformApplied> update(const TransformBatch&) noexcept;
        [[nodiscard]] RenderResult<MeshInstanceInformation> query(MeshInstanceId) const noexcept;
        [[nodiscard]] RenderResult<void> release(MeshInstancePublished) noexcept;

        [[nodiscard]] std::uint32_t recordCount() const noexcept
        {
            return static_cast<std::uint32_t>(shadow_.size());
        }

        [[nodiscard]] std::uint64_t writtenRecords() const noexcept
        {
            return written_records_;
        }

        [[nodiscard]] std::uint64_t checkedUpdates() const noexcept
        {
            return checked_updates_;
        }

        [[nodiscard]] std::uint64_t residentBytes() const noexcept;

    private:
        friend StorageFactory meshInstanceStorageFactory() noexcept;
        friend class MeshDraws;
        [[nodiscard]] RenderResult<void>
        prepare(const NativeResourcePlan&, const MeshInstanceBinding&, PreparedStorageBinding&) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<Backing>> allocate(std::size_t, VkBufferUsageFlags) noexcept;
        static void recordWrite(VkCommandBuffer, Buffer&, VkDeviceSize, std::span<const std::byte>) noexcept;
        [[nodiscard]] MeshInstanceDescription describe(const Entry&) const noexcept;
        void publish(std::unique_ptr<Backing>) noexcept;
        void retire(std::unique_ptr<Backing>&) noexcept;
        void unsubscribe(Use&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        MeshInstanceStorageConfig config_;
        cxx::SlotMap<Entry, MeshInstanceTag> entries_;
        std::vector<TransformMatrix> shadow_;
        std::unique_ptr<Backing> current_;
        std::vector<Page> pages_;
        std::vector<std::uint32_t> page_positions_;
        std::vector<Use*> uses_;
        std::map<MeshInstanceDrawGroup, std::vector<MeshDraws*>> draw_subscribers_;
        std::uint64_t resident_bytes_ = 0;
        std::uint64_t written_records_ = 0, checked_updates_ = 0;
    };
} // namespace lux::render::vulkan
