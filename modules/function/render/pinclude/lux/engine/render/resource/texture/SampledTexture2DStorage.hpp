#pragma once

#include <lux/engine/render/extensions/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/resources/texture/Resource.hpp>

#include <lux/cxx/container/StableSlotMap.hpp>
#include <map>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/resources/texture/Binding.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

namespace lux::render::detail
{

    class SampledTexture2DStorage final
    {
    public:
        using UploadResult = exec::task<RenderResult<TextureId>>;
        [[nodiscard]] UploadResult upload(TextureData) noexcept;
        [[nodiscard]] UploadResult upload(TexturePatchData) noexcept;

        using Descriptor = TextureResource;
        [[nodiscard]] static ResourceRegistration factory() noexcept;

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

        [[nodiscard]] static RenderResult<std::unique_ptr<SampledTexture2DStorage>>
        create(ResourceCreateContext&, ResourceInstanceId) noexcept;
        ~SampledTexture2DStorage() noexcept;
        SampledTexture2DStorage(const SampledTexture2DStorage&) = delete;
        SampledTexture2DStorage& operator=(const SampledTexture2DStorage&) = delete;

        [[nodiscard]] ResourceInstanceId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] RenderResult<void> release(TextureId) noexcept;
        [[nodiscard]] const SampledImage* find(TextureId) const noexcept;

    private:
        using ImagePreparation = std::pair<nri::Texture*, std::vector<ImageTransferRegion>>;
        SampledTexture2DStorage(ResourceCreateContext&, ResourceInstanceId) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        [[nodiscard]] RenderResult<ImagePreparation>
        prepareImage(const rdesc::TextureInfo&, std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<ImagePreparation>
        preparePatched(const SampledImage&, const TextureRegion&, std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<SampledImage> sampledImage(nri::Texture*, std::uint32_t color_space) noexcept;
        static void releaseImage(const nri::CoreInterface&, DescriptorTable&, SampledImage&) noexcept;
        [[nodiscard]] SampledImage* findEntry(TextureId) noexcept;
        void retire(SampledImage&) noexcept;
        DeviceSession& session_;
        ResourceUploader& uploads_;

        ResourceInstanceId id_;
        DescriptorTable* descriptors_ = nullptr;
        cxx::StableSlotMap<SampledImage, TextureTag> textures_;
        std::map<TextureId, std::uint32_t> patch_readers_;
    };
} // namespace lux::render::detail
