#pragma once
#include <lux/engine/render/resource/material/MeshPipelines.hpp>
#include <lux/engine/render/runtime/commands/Command.hpp>

#include <lux/engine/render/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>

#include <exec/task.hpp>
#include <memory>
#include <optional>
#include <unordered_map>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/description/MaterialLayout.hpp>
#include <lux/engine/render/resource/material/Binding.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/material/Resource.hpp>

namespace lux::render::detail
{
    struct MeshMaterialShader;
    class PreparedMeshPipelines;
    class MeshSurfacePrograms;
    class SampledTexture2DStorage;
    struct CpuDrawStorage;
    struct MeshProgramBinding;
    struct CompiledRasterStream;

    class MaterialStorage final
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

        struct MeshPurpose final
        {
            std::unique_ptr<MeshMaterialShader> shader;
            std::unique_ptr<const PreparedMeshPipelines> prepared;
            bool preparing = false;
        };

        struct ProgramSlot final
        {
            std::array<MeshPurpose, 3> mesh;
            Table* table = nullptr;
            std::size_t instances = 0;
        };

    public:
        [[nodiscard]] RenderResult<void> update(const MaterialUpdate&) noexcept;
        [[nodiscard]] RenderResult<void> updateColors(std::span<const MaterialColorUpdate>) noexcept;
        struct RegisteredProgram final
        {
            MaterialProgramId program;
            std::array<bool, 3> indexed_mesh_shader{};
        };

        [[nodiscard]] RenderResult<RegisteredProgram> registerProgram(rdesc::MaterialProgramSet) noexcept;
        [[nodiscard]] RenderResult<MaterialId> createInstance(const MaterialInstanceData&) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> prepareMeshPipelines(MaterialMeshPipelines) noexcept;
        [[nodiscard]] RenderResult<void> unregisterProgram(MaterialProgramId) noexcept;

        using Descriptor = MaterialResource;
        [[nodiscard]] static ResourceRegistration factory() noexcept;

        MaterialStorage(ResourceCreateContext&, ResourceInstanceId) noexcept;
        ~MaterialStorage() noexcept;
        MaterialStorage(const MaterialStorage&) = delete;
        MaterialStorage& operator=(const MaterialStorage&) = delete;

        [[nodiscard]] ResourceInstanceId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] MaterialProgramId programOf(MaterialId) const noexcept;
        [[nodiscard]] RenderResult<rdesc::MaterialGpuReference> gpuReference(MaterialId, std::uint32_t frame) const noexcept;
        struct RasterRecord final
        {
            rdesc::MaterialGpuReference reference;
            const nri::Buffer* buffer;
            GraphBufferRange bytes;
            std::span<const TextureId> textures;

            [[nodiscard]] RenderResult<rdesc::MaterialGpuReference>
            record(CpuDrawStorage&, RasterStreamRange programs, const SampledTexture2DStorage*) const noexcept;
        };

        [[nodiscard]] RenderResult<RasterRecord>
        selectRasterRecord(MaterialId, const RasterRecordLayout&) const noexcept;
        [[nodiscard]] std::shared_ptr<MeshSurfacePrograms>
        shareSurfacePrograms(const MeshProgramBinding&, const CompiledRasterStream&) const noexcept;
        [[nodiscard]] RenderResult<void> release(MaterialId) noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;

    private:
        friend class MeshSurfacePrograms;
        void invalidateSurfacePrograms(MaterialProgramId, rdesc::EMaterialPurpose) noexcept;
        [[nodiscard]] RenderResult<const PreparedMeshPipelines*>
        publishedMeshPipelines(MaterialProgramId, rdesc::EMaterialPurpose) const & noexcept;

        template <class Input>
        using WriteValue = std::conditional_t<std::same_as<Input, MaterialUpdate>, void, MaterialId>;
        template <class Input> [[nodiscard]] RenderResult<WriteValue<Input>> write(const Input&) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t, ResourceUploader&) noexcept;
        void finishFramePreparation(const ReplicaSettlement&) noexcept;
        void markDirty(Page&, std::uint32_t frame, std::uint32_t record) noexcept;
        void removeDirty(Page&, std::uint32_t frame) noexcept;
        [[nodiscard]] RenderResult<Page*> createPage(Table&) noexcept;
        void releasePage(Page&) noexcept;
        [[nodiscard]] ProgramSlot* findProgram(MaterialProgramId) noexcept;
        [[nodiscard]] const ProgramSlot* findProgram(MaterialProgramId) const noexcept;
        using RecordBindings = std::array<const rdesc::EDescriptorBindingInfo*, 3>;
        [[nodiscard]] RenderResult<Table*> qualifyTable(
            const rdesc::MaterialProgramSet&, const rdesc::ShaderBlock&, const RecordBindings&
        ) noexcept;

        void removeEmptyTable(Table&) noexcept;

        [[nodiscard]] RenderResult<TextureGpuRef> resolveTexture(TextureId) const noexcept;
        [[nodiscard]] const Entry* findEntry(MaterialId) const noexcept;
        [[nodiscard]] Entry* findEntry(MaterialId) noexcept;
        DeviceEntry& device_;
        // Owner-thread installation index. Only installed workspaces keep a selection alive.
        mutable std::vector<std::weak_ptr<MeshSurfacePrograms>> surface_programs_;
        std::vector<std::byte> record_scratch_;
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

        std::vector<ColorDestination> color_destinations_;
        cxx::StableSlotMap<Entry, MaterialTag> entries_;
        cxx::StableSlotMap<ProgramSlot, MaterialProgramTag> programs_;
        std::unordered_multimap<std::size_t, std::unique_ptr<Table>> tables_;
        std::array<std::vector<Page*>, 3> dirty_pages_;
    };
} // namespace lux::render::detail
