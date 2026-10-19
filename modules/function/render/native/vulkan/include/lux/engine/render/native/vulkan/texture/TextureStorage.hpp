#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/gapi/vk/Sampler.hpp>
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/retirement/Retirement.hpp>
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
        class TextureData final
        {
        public:
            TextureData(
                std::unique_ptr<Image>,
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
            std::unique_ptr<Image> image_;
            DescriptorRange descriptor_;
            rdesc::TextureInfo info_{};
            VkImageView view_ = VK_NULL_HANDLE;
            gapi::vk::Sampler sampler_;
            RetirementReservation retirement_;
            std::uint64_t revision_ = 0;
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
        [[nodiscard]] RenderResult<void> release(TextureId, std::uint64_t expected_revision) noexcept;
        [[nodiscard]] const TextureData* find(TextureId) const noexcept;
        // Owner-thread binding preparation; no independently owned Texture version is returned.
        [[nodiscard]] RenderResult<std::pair<rdesc::TextureInfo, std::uint32_t>>
        bind(TextureId, PreparedStorageBinding&) noexcept;

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
        struct Use;
        RenderResult<void> prepare(const NativeResourcePlan&, const TextureBinding&, PreparedStorageBinding&) noexcept;

        struct TextureRecord final
        {
            std::unique_ptr<TextureData> current;
            std::vector<Use*> uses;
            bool updating = false;
        };

        void unsubscribe(Use&) noexcept;
        void disconnect(TextureRecord&) noexcept;
        TextureStorage(DeviceSession& session, StorageId id) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        void rollback(Candidate&) noexcept;
        void retire(std::unique_ptr<TextureData>&) noexcept;
        void publish(Candidate&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        cxx::SlotMap<TextureRecord, TextureTag> textures_;
        DescriptorArray* descriptors_ = nullptr;
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
