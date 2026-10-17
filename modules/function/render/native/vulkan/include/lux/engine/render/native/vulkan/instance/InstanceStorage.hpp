#pragma once

#include <lux/engine/render/resource/Instance.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

namespace lux::render::vulkan
{
    struct NativeResourcePlan;
    struct PreparedStorageBinding;
    [[nodiscard]] StorageFactory instanceStorageFactory() noexcept;

    // Owner-thread sparse CPU authority and three mapped GPU snapshots. Busy means all actual
    // snapshots still have accepted GPU users; no writes or logical commits occur on that failure.
    class InstanceStorage final
    {
        struct Backing;
        struct Use;
        struct Physical;
        struct Group;
        struct Entry final
        {
            InstanceDescription description;
            std::uint64_t revision = 1;
            cxx::SlotKey<InstanceTag> key;
        };
        using Banks = std::array<std::unique_ptr<Backing>, 3>;

    public:
        InstanceStorage(DeviceSession&, StorageId, InstanceStorageConfig) noexcept;
        ~InstanceStorage() noexcept;
        InstanceStorage(const InstanceStorage&) = delete;
        InstanceStorage& operator=(const InstanceStorage&) = delete;
        [[nodiscard]] bool compatibleConfig(const InstanceStorageConfig& value) const noexcept { return value == config_; }
        [[nodiscard]] RenderResult<InstancePublished> create(const InstanceDescription&) noexcept;
        [[nodiscard]] RenderResult<TransformApplied> update(const TransformBatch&) noexcept;
        [[nodiscard]] RenderResult<InstanceInformation> query(InstanceId) const noexcept;
        [[nodiscard]] RenderResult<void> release(InstancePublished) noexcept;
        [[nodiscard]] std::uint32_t recordCount() const noexcept { return static_cast<std::uint32_t>(shadow_.size()); }
        [[nodiscard]] std::uint64_t writtenRecords() const noexcept { return written_records_; }
        [[nodiscard]] std::uint64_t checkedUpdates() const noexcept { return checked_updates_; }
        [[nodiscard]] std::uint64_t residentBytes() const noexcept;

    private:
        friend StorageFactory instanceStorageFactory() noexcept;
        [[nodiscard]] RenderResult<void> prepare(const NativeResourcePlan&, const InstanceBinding&,
            PreparedStorageBinding&) noexcept;
        [[nodiscard]] RenderResult<void> grow(std::size_t) noexcept;
        [[nodiscard]] RenderResult<Backing*> writable() noexcept;
        [[nodiscard]] RenderResult<void> write(Backing&, std::uint32_t, const TransformMatrix&) noexcept;
        [[nodiscard]] RenderResult<Group*> prepareGroup(const InstanceBinding&) noexcept;
        [[nodiscard]] RenderResult<Backing*> stageGroup(Group&, std::span<const std::uint32_t>) noexcept;
        [[nodiscard]] Group* findGroup(const InstanceDescription&) noexcept;
        void publishGroup(Group&, Backing&, std::vector<std::uint32_t>) noexcept;
        void publish(Backing&) noexcept;
        void retire(std::unique_ptr<Backing>&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        InstanceStorageConfig config_;
        cxx::SlotMap<Entry, InstanceTag> entries_;
        std::vector<TransformMatrix> shadow_;
        Banks banks_;
        Backing* current_ = nullptr;
        std::vector<Use*> uses_;
        std::vector<std::unique_ptr<Group>> groups_;
        std::shared_ptr<Physical> physical_;
        std::uint64_t written_records_ = 0, checked_updates_ = 0;
    };
}
