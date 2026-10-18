#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/Submission.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace lux::render::vulkan
{
    [[nodiscard]] StorageFactory textureStorageFactory() noexcept;
    struct PreparedStorageBinding;
    struct NativeResourcePlan;
    class TextureStorage;
    class ExecutionContext;

    class TextureStorage final
    {
        struct Residency;

        class TextureData final
        {
        public:
            TextureData(
                std::shared_ptr<Residency>,
                std::shared_ptr<Image>,
                DescriptorRange,
                const rdesc::TextureInfo&,
                std::uint64_t revision
            ) noexcept;
            ~TextureData() noexcept;
            TextureData(const TextureData&) = delete;
            TextureData& operator=(const TextureData&) = delete;

            [[nodiscard]] const rdesc::TextureInfo& info() const noexcept
            {
                return info_;
            }

            [[nodiscard]] std::uint32_t descriptorIndex() const noexcept
            {
                return descriptor_.index();
            }

            [[nodiscard]] const Image& image() const noexcept
            {
                return *image_;
            }

            [[nodiscard]] std::uint64_t revision() const noexcept
            {
                return revision_;
            }

        private:
            friend class TextureStorage;
            std::shared_ptr<Residency> residency_;
            std::shared_ptr<Image> image_;
            DescriptorRange descriptor_;
            rdesc::TextureInfo info_{};
            VkImageView view_ = VK_NULL_HANDLE;
            VkSampler sampler_ = VK_NULL_HANDLE;
            // Clone layout transitions must still order against prior Draws. This is not a retirement right.
            SubmissionRequirements last_use_;
            std::uint64_t bytes_ = 0, revision_ = 0;
        };

    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<TextureStorage>> create(
            DeviceSession& session,
            StorageId id,
            const TextureStorageConfig& config
        ) noexcept;

        [[nodiscard]] bool compatibleConfig(const TextureStorageConfig& config) const noexcept
        {
            return config_ == config;
        }

        [[nodiscard]] StorageId id() const noexcept
        {
            return id_;
        }

        ~TextureStorage() noexcept;
        TextureStorage(const TextureStorage&) = delete;
        TextureStorage& operator=(const TextureStorage&) = delete;
        [[nodiscard]] RenderResult<void> release(TextureId, std::uint64_t expected_revision) noexcept;
        [[nodiscard]] RenderResult<std::shared_ptr<const TextureData>> acquire(TextureId) noexcept;
        [[nodiscard]] const TextureData* find(TextureId) const noexcept;
        // Physical content reference independent of the logical Storage lifetime.
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
        using UploadResult = exec::task<RenderResult<TexturePublished>>;
        [[nodiscard]] UploadResult upload(TextureWrite) noexcept;
        [[nodiscard]] UploadResult upload(TextureRegionWrite) noexcept;
        using ImagePreparation = std::pair<std::shared_ptr<Image>, std::vector<ImageTransferRegion>>;
        [[nodiscard]] RenderResult<ImagePreparation> prepareImage(
            const rdesc::TextureInfo&,
            std::span<const std::byte>,
            std::optional<TextureRegion> = {}
        ) noexcept;
        [[nodiscard]] RenderResult<void> admit(std::uint64_t minimum_bytes, bool creates_identity) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<Candidate>> reserve(
            std::shared_ptr<Image>,
            const rdesc::TextureInfo&,
            TextureId,
            std::uint64_t expected_revision
        ) noexcept;
        struct Use;
        RenderResult<void> prepare(const NativeResourcePlan&, const TextureBinding&, PreparedStorageBinding&) noexcept;

        struct TextureRecord final
        {
            std::shared_ptr<TextureData> current;
            std::vector<Use*> uses;
            bool updating = false;
        };

        void unsubscribe(Use&) noexcept;
        void disconnect(TextureRecord&) noexcept;
        TextureStorage(DeviceSession& session, StorageId id, TextureStorageConfig config) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        void rollback(Candidate&) noexcept;
        void publish(Candidate&, const SubmissionRequirements&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        cxx::SlotMap<TextureRecord, TextureTag> textures_;
        std::shared_ptr<Residency> residency_;
        std::shared_ptr<DescriptorArray> descriptors_;
        TextureStorageConfig config_;
    };
} // namespace lux::render::vulkan

namespace lux::render
{
    template <> struct StorageTraits<vulkan::TextureStorage> final
    {
        using Result = exec::task<RenderResult<TexturePublished>>;
        [[nodiscard]] static Result upload(vulkan::TextureStorage&, TextureWrite) noexcept;
        [[nodiscard]] static Result upload(vulkan::TextureStorage&, TextureRegionWrite) noexcept;
    };
} // namespace lux::render
