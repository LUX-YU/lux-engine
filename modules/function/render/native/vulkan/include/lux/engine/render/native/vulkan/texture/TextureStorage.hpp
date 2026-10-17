#pragma once

#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Upload.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <lux/engine/render/native/vulkan/retirement/Retirement.hpp>

namespace lux::render::vulkan
{
    [[nodiscard]] StorageFactory textureStorageFactory() noexcept;
    struct PreparedStorageBinding;
    struct NativeResourcePlan;
    class TextureStorage;
    class ExecutionContext;
    struct TextureDescriptorTag;

    class TextureStorage final
    {
        struct Physical;
        struct RetiredBacking;
        class Backing final
        {
        public:
            Backing() noexcept;
            ~Backing() noexcept;
            Backing(const Backing&) = delete;
            Backing& operator=(const Backing&) = delete;
            [[nodiscard]] const rdesc::TextureInfo& info() const noexcept { return info_; }
            [[nodiscard]] std::uint32_t descriptorIndex() const noexcept { return descriptor_.index; }
            [[nodiscard]] const ImageAllocation& image() const noexcept { return *image_; }
            [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
        private:
            friend class TextureStorage;
            friend struct Physical;
            friend class DeviceSession;
            friend class ExecutionContext;
            cxx::SlotKey<TextureDescriptorTag> key_;
            DescriptorArray::Slot descriptor_;
            rdesc::TextureInfo info_{};
            ImagePermit permit_;
            const ImageAllocation* image_ = nullptr;
            VkImageView view_ = VK_NULL_HANDLE;
            VkSampler sampler_ = VK_NULL_HANDLE;
            SubmissionRequirements last_use_;
            std::uint64_t bytes_ = 0, revision_ = 0;
            std::uint32_t leases_ = 0;
            bool live_ = false, reserved_ = true;
            RetirementReservation retirement_;
            std::unique_ptr<RetiredBacking> retired_;
        };

    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<TextureStorage>>
        create(DeviceSession& session, StorageId id, const TextureStorageConfig& config) noexcept;
        [[nodiscard]] bool compatibleConfig(const TextureStorageConfig& config) const noexcept
        {
            return config_ == config;
        }

        [[nodiscard]] StorageId id() const noexcept { return id_; }
        ~TextureStorage() noexcept;
        TextureStorage(const TextureStorage&) = delete;
        TextureStorage& operator=(const TextureStorage&) = delete;
        [[nodiscard]] RenderResult<void> release(TextureId, std::uint64_t expected_revision) noexcept;
        [[nodiscard]] RenderResult<Backing*> acquire(TextureId) noexcept;
        void releaseLease(Backing& version) noexcept;
        [[nodiscard]] const Backing* find(TextureId) const noexcept;
        // Professional immutable content reference. Caller holds this Storage's existing StorageLease.
        // The returned reference does not follow later publications of the same logical TextureId.
        [[nodiscard]] RenderResult<std::pair<rdesc::TextureInfo, std::uint32_t>>
        pin(TexturePublished, PreparedStorageBinding&) noexcept;
        [[nodiscard]] VkDescriptorSetLayout layout() const noexcept;
        [[nodiscard]] VkDescriptorSet descriptors() const noexcept;
        [[nodiscard]] std::size_t residentVersions() const noexcept;
        [[nodiscard]] std::uint64_t residentBytes() const noexcept;
        [[nodiscard]] std::uint64_t residentPeak() const noexcept;
    private:
        friend class DeviceSession;
        friend StorageFactory textureStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<TextureStorage>;
        struct Candidate;
        using UploadResult = RenderResult<std::pair<TexturePublished, GpuWork>>;
        [[nodiscard]] UploadResult upload(const TextureWrite&) noexcept;
        [[nodiscard]] UploadResult upload(const TextureRegionWrite&) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<Transfer>> prepareImage(const rdesc::TextureInfo&,
            std::span<const std::byte>, std::optional<TextureRegion> = {}, Backing* base = nullptr) noexcept;
        [[nodiscard]] RenderResult<void> admit(std::uint64_t minimum_bytes, bool creates_identity) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<Candidate>> reserve(Transfer&, const rdesc::TextureInfo&,
            TextureId, std::uint64_t expected_revision) noexcept;
        struct Use;
        RenderResult<void> prepare(const NativeResourcePlan&, const TextureBinding&, PreparedStorageBinding&) noexcept;
        std::vector<Use*> uses_; // non-owning subscriptions, changed only at Attach/publication/detach
        struct Record final { Backing* current = nullptr; bool updating = false; };
        TextureStorage(DeviceSession& session, StorageId id, TextureStorageConfig config) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        void rollback(Candidate&) noexcept;
        void publish(Candidate&, Transfer&) noexcept;
        void retireUnused(Backing&) noexcept;
        void destroy(Backing&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        cxx::SlotMap<Record, TextureTag> textures_;
        std::shared_ptr<Physical> physical_;
        TextureStorageConfig config_;
    };
}

namespace lux::render
{
    template<> struct StorageTraits<vulkan::TextureStorage> final
    {
        using Result = RenderResult<std::pair<TexturePublished, vulkan::GpuWork>>;
        [[nodiscard]] static Result upload(vulkan::TextureStorage&, const TextureWrite&) noexcept;
        [[nodiscard]] static Result upload(vulkan::TextureStorage&, const TextureRegionWrite&) noexcept;
    };
}
