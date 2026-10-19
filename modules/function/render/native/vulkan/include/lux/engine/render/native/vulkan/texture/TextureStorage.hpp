#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Texture.hpp>
#include <lux/engine/render/resource/Upload.hpp>

namespace lux::render::vulkan
{
    [[nodiscard]] StorageFactory textureStorageFactory() noexcept;
    struct NativeResourcePlan;

    class TextureStorage final
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
            friend class TextureStorage;
            void release(VkDevice, VmaAllocator, DescriptorArray&) noexcept;
            gapi::vk::VmaImage image_;
            VkImageView view_ = VK_NULL_HANDLE;
            gapi::vk::Sampler sampler_;
            DescriptorArray::Slot descriptor_{};
            TextureGpuRef reference_{};
        };

        [[nodiscard]] static RenderResult<std::unique_ptr<TextureStorage>> create(DeviceSession&, StorageId) noexcept;
        ~TextureStorage() noexcept;
        TextureStorage(const TextureStorage&) = delete;
        TextureStorage& operator=(const TextureStorage&) = delete;

        [[nodiscard]] StorageId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] RenderResult<void> release(TextureId) noexcept;
        [[nodiscard]] const SampledImage* find(TextureId) const noexcept;

    private:
        friend StorageFactory textureStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<TextureStorage>;
        using UploadResult = exec::task<RenderResult<TextureId>>;
        using ImagePreparation = std::pair<gapi::vk::VmaImage, std::vector<ImageTransferRegion>>;
        TextureStorage(DeviceSession&, StorageId) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        [[nodiscard]] UploadResult upload(TextureWrite) noexcept;
        [[nodiscard]] UploadResult upload(TextureRegionWrite) noexcept;
        [[nodiscard]] RenderResult<ImagePreparation>
        prepareImage(const rdesc::TextureInfo&, std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<ImagePreparation>
        preparePatched(const SampledImage&, const TextureRegion&, std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<SampledImage> sampledImage(gapi::vk::VmaImage, std::uint32_t color_space) noexcept;
        [[nodiscard]] RenderResult<StorageResource>
        resolve(const NativeResourcePlan&, const TextureBinding&, StorageSubscription&) noexcept;
        void retire(SampledImage&) noexcept;
        DeviceSession& session_;
        StorageId id_;
        DescriptorArray* descriptors_ = nullptr;
        cxx::StableSlotMap<SampledImage, TextureTag> textures_;
    };
} // namespace lux::render::vulkan

namespace lux::render
{
    template <> struct StorageTraits<vulkan::TextureStorage> final
    {
        using Result = exec::task<RenderResult<TextureId>>;
        [[nodiscard]] static Result upload(vulkan::TextureStorage&, TextureWrite) noexcept;
        [[nodiscard]] static Result upload(vulkan::TextureStorage&, TextureRegionWrite) noexcept;
    };
} // namespace lux::render
