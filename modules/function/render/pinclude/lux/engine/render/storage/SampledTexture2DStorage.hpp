#pragma once

#include <lux/engine/render/extensions/execution/FrameExecution.hpp>
#include <lux/engine/render/extensions/storage/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/storage/StorageCreateContext.hpp>

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/storage/SampledTexture2D.hpp>
#include <lux/engine/render/extensions/storage/StorageBinding.hpp>
#include <lux/engine/render/extensions/storage/StorageFactory.hpp>
#include <lux/engine/render/extensions/storage/Upload.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

namespace lux::render::detail
{

    class SampledTexture2DStorage final
    {
    public:
        using Descriptor = SampledTexture2DStorageDescriptor;
        [[nodiscard]] static StorageFactory factory() noexcept;

        // Stable SlotMap values: actual GPU objects and shader input, no asset layout or parent control.
        class SampledImage final
        {
        public:
            SampledImage() noexcept = default;
            SampledImage(const SampledImage&) = delete;
            SampledImage& operator=(const SampledImage&) = delete;
            SampledImage(SampledImage&&) noexcept;
            SampledImage& operator=(SampledImage&&) = delete;

            [[nodiscard]] const TextureGpuRef& gpuReference() const noexcept
            {
                return reference_;
            }

            [[nodiscard]] const nri::Texture& image() const noexcept
            {
                return *image_;
            }

        private:
            friend class SampledTexture2DStorage;
            nri::Texture* image_ = nullptr;
            nri::Descriptor* view_ = nullptr;
            DescriptorTable::Slot descriptor_{};
            TextureGpuRef reference_{};
        };

        [[nodiscard]] static RenderResult<std::unique_ptr<SampledTexture2DStorage>> create(
            StorageCreateContext&,
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
        friend struct lux::render::StorageTraits<SampledTexture2DStorage>;
        using UploadResult = exec::task<RenderResult<TextureId>>;
        using ImagePreparation = std::pair<nri::Texture*, std::vector<ImageTransferRegion>>;
        SampledTexture2DStorage(StorageCreateContext&, StorageId) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        [[nodiscard]] UploadResult upload(TextureWrite) noexcept;
        [[nodiscard]] UploadResult upload(TextureRegionWrite) noexcept;
        [[nodiscard]] RenderResult<ImagePreparation>
        prepareImage(const rdesc::TextureInfo&, std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<ImagePreparation>
        preparePatched(const SampledImage&, const TextureRegion&, std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<SampledImage> sampledImage(nri::Texture*, std::uint32_t color_space) noexcept;
        [[nodiscard]] RenderResult<ResourceInput>
        resolve(const VGraphResourceDesc&, const TextureBinding&, StorageSubscription&) noexcept;
        static void releaseImage(const nri::CoreInterface&, DescriptorTable&, SampledImage&) noexcept;
        void retire(SampledImage&) noexcept;
        DeviceSession& session_;
        ResourceUploader& uploads_;
        FrameExecution& frames_;

        StorageId id_;
        DescriptorTable* descriptors_ = nullptr;
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
