#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/device/DescriptorArray.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/StorageFactory.hpp>
#include <lux/engine/render/resource/Upload.hpp>
#include <lux/engine/render/storage/SampledTexture2D.hpp>
#include <lux/engine/render/upload/UploadContext.hpp>

namespace lux::render::detail
{
    [[nodiscard]] StorageFactory textureStorageFactory() noexcept;
    struct CompiledResource;

    class SampledTexture2DStorage final
    {
    public:
        // Stable SlotMap values: actual GPU objects and shader input, no asset layout or parent owner.
        class SampledImage final
        {
        public:
            SampledImage() noexcept = default;
            SampledImage(const SampledImage&) = delete;
            SampledImage& operator=(const SampledImage&) = delete;
            SampledImage(SampledImage&&) noexcept;
            SampledImage& operator=(SampledImage&&) = delete;

            [[nodiscard]] std::uint32_t descriptorIndex() const noexcept
            {
                return descriptor_.index;
            }

            [[nodiscard]] const gapi::vk::VmaImage& image() const noexcept
            {
                return image_;
            }

        private:
            friend class SampledTexture2DStorage;
            void release(VkDevice, VmaAllocator, DescriptorArray&) noexcept;
            gapi::vk::VmaImage image_;
            VkImageView view_ = VK_NULL_HANDLE;
            gapi::vk::Sampler sampler_;
            DescriptorArray::Slot descriptor_{};
            TextureGpuRef reference_{};
        };

        [[nodiscard]] static RenderResult<std::unique_ptr<SampledTexture2DStorage>> create(
            DeviceSession&,
            StorageId
        ) noexcept;
        ~SampledTexture2DStorage() noexcept;
        SampledTexture2DStorage(const SampledTexture2DStorage&) = delete;
        SampledTexture2DStorage& operator=(const SampledTexture2DStorage&) = delete;

        [[nodiscard]] StorageId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] RenderResult<void> release(TextureId) noexcept;
        [[nodiscard]] const SampledImage* find(TextureId) const noexcept;

    private:
        friend StorageFactory textureStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<SampledTexture2DStorage>;
        using UploadResult = exec::task<RenderResult<TextureId>>;
        using ImagePreparation = std::pair<gapi::vk::VmaImage, std::vector<ImageTransferRegion>>;
        SampledTexture2DStorage(DeviceSession&, StorageId) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        [[nodiscard]] UploadResult upload(TextureWrite) noexcept;
        [[nodiscard]] UploadResult upload(TextureRegionWrite) noexcept;
        [[nodiscard]] RenderResult<ImagePreparation>
        prepareImage(const rdesc::TextureInfo&, std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<ImagePreparation>
        preparePatched(const SampledImage&, const TextureRegion&, std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<SampledImage> sampledImage(gapi::vk::VmaImage, std::uint32_t color_space) noexcept;
        [[nodiscard]] RenderResult<StorageResource>
        resolve(const CompiledResource&, const TextureBinding&, StorageSubscription&) noexcept;
        void retire(SampledImage&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        DescriptorArray* descriptors_ = nullptr;
        cxx::StableSlotMap<SampledImage, TextureTag> textures_;
    };
} // namespace lux::render::detail

namespace lux::render
{
    template <> struct StorageTraits<detail::SampledTexture2DStorage> final
    {
        using Result = exec::task<RenderResult<TextureId>>;
        [[nodiscard]] static Result upload(detail::SampledTexture2DStorage&, TextureWrite) noexcept;
        [[nodiscard]] static Result upload(detail::SampledTexture2DStorage&, TextureRegionWrite) noexcept;
    };
} // namespace lux::render
