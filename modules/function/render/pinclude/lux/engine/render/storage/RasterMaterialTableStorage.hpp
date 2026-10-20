#pragma once

#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/StorageCreateContext.hpp>

#include <exec/task.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/Upload.hpp>
#include <lux/engine/render/storage/RasterMaterialTable.hpp>

namespace lux::render::detail
{
    [[nodiscard]] StorageFactory materialStorageFactory() noexcept;

    class RasterMaterialTableStorage final
    {
        struct Table;

        struct Entry final
        {
            Table* table = nullptr;
            std::uint32_t slot = 0;
            StorageSubscription* bindings = nullptr;
        };

    public:
        RasterMaterialTableStorage(StorageCreateContext&, StorageId) noexcept;
        ~RasterMaterialTableStorage() noexcept;
        RasterMaterialTableStorage(const RasterMaterialTableStorage&) = delete;
        RasterMaterialTableStorage& operator=(const RasterMaterialTableStorage&) = delete;

        [[nodiscard]] StorageId id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] RenderResult<std::uint32_t> gpuIndex(MaterialId) const noexcept;
        [[nodiscard]] bool sameTable(MaterialId, MaterialId) const noexcept;
        [[nodiscard]] RenderResult<void> release(MaterialId) noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;

    private:
        friend StorageFactory materialStorageFactory() noexcept;
        friend struct lux::render::StorageTraits<RasterMaterialTableStorage>;
        template <class Input>
        using WriteValue = std::conditional_t<std::same_as<Input, MaterialWrite>, MaterialId, Ack>;
        template <class Input> [[nodiscard]] exec::task<RenderResult<WriteValue<Input>>> write(Input) noexcept;
        [[nodiscard]] exec::task<RenderResult<MaterialId>> upload(MaterialWrite) noexcept;
        [[nodiscard]] exec::task<RenderResult<Ack>> upload(MaterialUpdate) noexcept;
        [[nodiscard]] RenderResult<void>
        resolve(const CompiledResource&, const MaterialBinding&, StorageSubscription&, std::span<StorageResource>) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t, ResourceUploader&) noexcept;
        void markDirty(Table&, std::uint32_t frame, std::uint32_t record) noexcept;
        void removeDirty(Table&, std::uint32_t frame) noexcept;
        void releaseTable(Table&) noexcept;
        [[nodiscard]] RenderResult<Table*> table(MaterialProgram&) noexcept;
        [[nodiscard]] std::uint32_t textureIndex(TextureId) const noexcept;
        static void publish(Table&, std::uint32_t frame) noexcept;
        DeviceSession& session_;
        ResourceUploader& uploads_;
        FrameExecution& frame_execution_;
        ResourceRegistry& resources_;

        StorageId id_;
        cxx::StableSlotMap<Entry, MaterialTag> entries_;
        std::vector<std::unique_ptr<Table>> tables_;
        std::array<std::vector<Table*>, 3> dirty_tables_;
    };
} // namespace lux::render::detail

namespace lux::render
{
    template <> struct StorageTraits<detail::RasterMaterialTableStorage> final
    {
        [[nodiscard]] static exec::task<RenderResult<MaterialId>> upload(
            detail::RasterMaterialTableStorage&,
            MaterialWrite
        ) noexcept;
        [[nodiscard]] static exec::task<RenderResult<Ack>> upload(
            detail::RasterMaterialTableStorage&,
            MaterialUpdate
        ) noexcept;
    };
} // namespace lux::render
