#pragma once
#include <lux/engine/render/runtime/commands/MaterialCommands.hpp>
#include <lux/engine/render/runtime/commands/Command.hpp>

#include <lux/engine/render/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/material/ProgramSource.hpp>

#include <exec/task.hpp>
#include <memory>
#include <optional>
#include <unordered_map>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/description/MaterialLayout.hpp>
#include <lux/engine/render/resource/material/Binding.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/material/MaterialProgramRecord.hpp>
#include <lux/engine/render/resource/material/Resource.hpp>

namespace lux::render::detail
{

    class RasterMaterialStorage final
    {
        struct Table;
        struct Page;

        struct Entry final
        {
            MaterialProgramId program;
            Page* page = nullptr;
            std::uint32_t slot = 0;
            std::vector<TextureId> textures;
        };

        struct ProgramSlot final
        {
            explicit ProgramSlot(DeviceSession& device) noexcept : ready(device) {}
            MaterialProgramRecord record;
            ResourceProgramUses ready;
            Table* table = nullptr;
            std::size_t instances = 0;
        };

    public:
        [[nodiscard]] exec::task<RenderResult<Ack>> upload(ReplicaMutation&, MaterialUpdate) noexcept;
        [[nodiscard]] RenderResult<void> updateColors(std::span<const MaterialColorUpdate>) noexcept;
        [[nodiscard]] RenderResult<MaterialProgramId> registerProgram(rdesc::MaterialProgramSet) noexcept;
        [[nodiscard]] exec::task<RenderResult<MaterialId>>
        createInstance(ReplicaMutation&, MaterialCommands::CreateInstance::Request) noexcept;
        [[nodiscard]] RenderResult<void> unregisterProgram(MaterialProgramId) noexcept;

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
        struct RasterRecord final
        {
            rdesc::MaterialGpuReference reference;
            ResourceInput buffer;
            std::span<const TextureId> textures;
        };

        [[nodiscard]] RenderResult<RasterRecord>
        selectRasterRecord(MaterialId, std::uint32_t frame, const rdesc::ShaderBlock& layout) const noexcept;
        [[nodiscard]] MaterialProgramId programId(MaterialId) const noexcept;
        [[nodiscard]] ResourceProgramUses* readyPrograms(MaterialProgramId) noexcept;
        [[nodiscard]] RenderResult<SelectedMaterialProgram>
        programInput(MaterialProgramId) const & noexcept;
        [[nodiscard]] RenderResult<void> release(MaterialId) noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;

    private:
        template <class Input>
        using WriteValue = std::conditional_t<std::same_as<Input, MaterialUpdate>, Ack, MaterialId>;
        template <class Input> [[nodiscard]] exec::task<RenderResult<WriteValue<Input>>>
        write(ReplicaMutation&, Input) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t, ResourceUploader&) noexcept;
        void finishFramePreparation(const ReplicaSettlement&) noexcept;
        void markDirty(Page&, std::uint32_t frame, std::uint32_t record) noexcept;
        void removeDirty(Page&, std::uint32_t frame) noexcept;
        [[nodiscard]] RenderResult<Page*> createPage(Table&) noexcept;
        void releasePage(Page&) noexcept;
        [[nodiscard]] ProgramSlot* findProgram(MaterialProgramId) noexcept;
        [[nodiscard]] const ProgramSlot* findProgram(MaterialProgramId) const noexcept;
        [[nodiscard]] RenderResult<Table*> qualifyTable(
            const rdesc::MaterialProgramSet&, const rdesc::ShaderBlock&,
            std::array<std::optional<rdesc::EDescriptorBindingInfo>, 3>
        ) noexcept;
        void removeProgram(ProgramSlot&) noexcept;

        void removeEmptyTable(Table&) noexcept;

        struct TextureInput final
        {
            TextureGpuRef reference;
            const nri::Texture* image = nullptr;
        };

        [[nodiscard]] RenderResult<TextureInput> resolveTexture(TextureId) const noexcept;
        [[nodiscard]] const Entry* findEntry(MaterialId) const noexcept;
        [[nodiscard]] Entry* findEntry(MaterialId) noexcept;
        DeviceSession& session_;
        bool has_uploads_;
        std::uint32_t frame_count_;
        ResourceRegistry& resources_;

        ResourceInstanceId id_;
        struct ColorDestination final
        {
            Page* page;
            std::size_t offset;
            std::uint32_t record;
        };

        std::uint64_t preparation_revision_ = 1;
        std::vector<ColorDestination> color_destinations_;
        cxx::StableSlotMap<Entry, MaterialTag> entries_;
        cxx::StableSlotMap<ProgramSlot, MaterialProgramTag> programs_;
        std::unordered_multimap<std::size_t, std::unique_ptr<Table>> tables_;
        std::array<std::vector<Page*>, 3> dirty_pages_;
    };
} // namespace lux::render::detail
