#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/gapi/vk/Sampler.hpp>
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/retirement/Retirement.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace lux::render::vulkan
{
    [[nodiscard]] StorageFactory textureStorageFactory() noexcept;
    struct NativeResourcePlan;
    class TextureStorage;
    class ExecutionContext;

    class TextureStorage final
    {
    public:
        class SampledImage final
        {
        public:
            SampledImage(
                std::unique_ptr<Image>,
                DescriptorRange,
                const rdesc::TextureInfo&,
                std::uint64_t revision
            ) noexcept;
            ~SampledImage() noexcept;
            SampledImage(const SampledImage&) = delete;
            SampledImage& operator=(const SampledImage&) = delete;
            SampledImage(SampledImage&&) noexcept;
            SampledImage& operator=(SampledImage&&) noexcept;

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
            void releaseViews() noexcept;
            std::unique_ptr<Image> image_;
            DescriptorRange descriptor_;
            rdesc::TextureInfo info_{};
            VkImageView view_ = VK_NULL_HANDLE;
            gapi::vk::Sampler sampler_;
            RetirementReservation retirement_;
            std::uint64_t revision_ = 0;
            TextureGpuRef reference_;
        };

    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<TextureStorage>> create(
            DeviceSession& session,
            StorageId id
        ) noexcept;

        [[nodiscard]] StorageId id() const noexcept
        {
            return id_;
        }

        ~TextureStorage() noexcept;
        TextureStorage(const TextureStorage&) = delete;
        TextureStorage& operator=(const TextureStorage&) = delete;
        [[nodiscard]] exec::task<RenderResult<Ack>> release(TextureId, std::uint64_t expected_revision) noexcept;
        [[nodiscard]] const SampledImage* find(TextureId) const noexcept;

        // One non-owning dependency per interested Storage, not a per-Attach heap Use. Both callbacks
        // run in the Texture Sender's uninterrupted commit segment; only Material writes its own table.
        class Observer final
        {
        public:
            Observer() noexcept = default;
            ~Observer() noexcept;
            Observer(const Observer&) = delete;
            Observer& operator=(const Observer&) = delete;
            void* receiver = nullptr;
            bool (*affected)(const void*, TextureId) noexcept = nullptr;
            void (*record)(void*, VkCommandBuffer, TextureId, std::uint32_t) noexcept = nullptr;
            void (*adopt)(void*, TextureId, std::uint32_t) noexcept = nullptr;
            void reset() noexcept;

        private:
            friend class TextureStorage;
            TextureStorage* source_ = nullptr;
            std::size_t position_ = 0;
        };

        void observe(Observer&) noexcept;

    private:
        friend class DeviceSession;
        friend StorageFactory textureStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<TextureStorage>;
        struct Candidate;
        using UploadResult = exec::task<RenderResult<TexturePublished>>;
        [[nodiscard]] UploadResult upload(TextureWrite) noexcept;
        [[nodiscard]] UploadResult upload(TextureRegionWrite) noexcept;
        using ImagePreparation = std::pair<std::unique_ptr<Image>, std::vector<ImageTransferRegion>>;
        [[nodiscard]] RenderResult<ImagePreparation> prepareImage(
            const rdesc::TextureInfo&,
            std::span<const std::byte>,
            std::optional<TextureRegion> = {}
        ) noexcept;
        [[nodiscard]] RenderResult<Candidate> reserve(
            std::unique_ptr<Image>,
            const rdesc::TextureInfo&,
            TextureId,
            std::uint64_t expected_revision
        ) noexcept;
        [[nodiscard]] RenderResult<StorageResource>
        resolve(const NativeResourcePlan&, const TextureBinding&, StorageSubscription&) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> commit(Candidate&) noexcept;
        [[nodiscard]] bool hasDependents(TextureId) const noexcept;
        void recordDependents(VkCommandBuffer, TextureId, std::uint32_t) noexcept;
        void adoptDependents(TextureId, std::uint32_t) noexcept;
        [[nodiscard]] static StorageResource resource(const SampledImage*) noexcept;

        struct TextureRecord final
        {
            std::unique_ptr<SampledImage> current;
            StorageSubscription* bindings = nullptr;
            bool updating = false;
        };

        void disconnect(TextureRecord&) noexcept;
        TextureStorage(DeviceSession& session, StorageId id) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        void rollback(Candidate&) noexcept;
        void retire(std::unique_ptr<SampledImage>&) noexcept;
        void publish(Candidate&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        cxx::StableSlotMap<TextureRecord, TextureTag> textures_;
        DescriptorArray* descriptors_ = nullptr;
        std::vector<Observer*> observers_;
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
