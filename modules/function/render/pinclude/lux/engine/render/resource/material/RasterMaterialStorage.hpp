#pragma once
#include <lux/engine/render/commands/Command.hpp>

#include <lux/engine/render/extensions/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>

#include <exec/task.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/description/MaterialLayout.hpp>
#include <lux/engine/render/resources/material/Binding.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/resources/material/Resource.hpp>

namespace lux::render::detail
{

    class RasterMaterialStorage final
    {
        struct Table;
        struct Page;

        struct Entry final
        {
            Page* page = nullptr;
            std::uint32_t slot = 0;
            StorageSubscription* bindings = nullptr;
            std::vector<const nri::Texture*> images;
        };

    public:
        [[nodiscard]] exec::task<RenderResult<MaterialId>> upload(MaterialData) noexcept;
        [[nodiscard]] exec::task<RenderResult<Ack>> upload(MaterialUpdate) noexcept;

        using Descriptor = MaterialResource;
        [[nodiscard]] static ResourceRegistration factory() noexcept;

        RasterMaterialStorage(ResourceCreateContext&, ResourceInstanceId) noexcept;
        ~RasterMaterialStorage() noexcept;
        RasterMaterialStorage(const RasterMaterialStorage&) = delete;
        RasterMaterialStorage& operator=(const RasterMaterialStorage&) = delete;

        [[nodiscard]] ResourceInstanceId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] RenderResult<rdesc::MaterialGpuReference>
        gpuReference(MaterialId, std::uint32_t frame) const noexcept;
        // Cold immutable program borrow. Copy the selected compilation input before suspending;
        // this borrow neither keeps a material alive nor grants access to the parameter replicas.
        [[nodiscard]] const rdesc::MaterialProgramSet* program(MaterialId) const noexcept;
        // Append one definition's actual page and image reads for this FIF; no descriptor/Graph ownership.
        [[nodiscard]] RenderResult<void>
        collectReads(
            MaterialId program_source,
            std::uint32_t frame,
            std::vector<ResourceInput>& buffers,
            std::vector<const nri::Texture*>& images
        ) const noexcept;

        struct ImageUse final
        {
            const nri::Texture* image;
            std::uint32_t records;
        };

        [[nodiscard]] RenderResult<void> release(MaterialId) noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;

    private:
        template <class Input>
        using WriteValue = std::conditional_t<std::same_as<Input, MaterialData>, MaterialId, Ack>;
        template <class Input> [[nodiscard]] exec::task<RenderResult<WriteValue<Input>>> write(Input) noexcept;
        [[nodiscard]] RenderResult<void>
        resolve(const VGraphResourceDesc&, const MaterialBinding&, StorageSubscription&, std::span<ResourceInput>) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t, ResourceUploader&, Preparation&) noexcept;
        void finishFramePreparation(std::uint32_t frame, RenderResult<void>) noexcept;
        void markDirty(Page&, std::uint32_t frame, std::uint32_t record) noexcept;
        void removeDirty(Page&, std::uint32_t frame) noexcept;
        [[nodiscard]] RenderResult<Page*> createPage(Table&) noexcept;
        void releasePage(Page&) noexcept;
        [[nodiscard]] RenderResult<Table*> table(rdesc::MaterialProgramSet&) noexcept;

        struct TextureInput final
        {
            TextureGpuRef reference;
            const nri::Texture* image = nullptr;
        };

        [[nodiscard]] RenderResult<TextureInput> resolveTexture(TextureId) const noexcept;
        static void addImageUses(Table&, std::span<const nri::Texture* const>) noexcept;
        static void removeImageUses(Table&, std::span<const nri::Texture* const>) noexcept;
        [[nodiscard]] const Entry* findEntry(MaterialId) const noexcept;
        [[nodiscard]] Entry* findEntry(MaterialId) noexcept;
        DeviceSession& session_;
        ResourceUploader& uploads_;
        std::uint32_t frame_count_;
        ResourceRegistry& resources_;

        ResourceInstanceId id_;
        std::uint64_t preparation_revision_ = 1;
        cxx::StableSlotMap<Entry, MaterialTag> entries_;
        std::vector<std::unique_ptr<Table>> tables_;
        std::array<std::vector<Page*>, 3> dirty_pages_;
    };
} // namespace lux::render::detail
