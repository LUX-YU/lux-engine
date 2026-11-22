#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/execution/PreparedCpuValue.hpp>
#include <lux/engine/render/extensions/execution/StorageSnapshot.hpp>
#include <lux/engine/render/graph/DrawList.hpp>
#include <lux/engine/render/extensions/graph/ConstantBlock.hpp>
#include <lux/engine/render/extensions/graph/GraphError.hpp>
#include <lux/engine/render/resources/material/Data.hpp>
#include <lux/engine/render/resources/mesh/Data.hpp>
#include <lux/engine/render/resources/texture/Data.hpp>
#include <array>
#include <bit>
#include <cstring>
#include <functional>
#include <span>

namespace lux::render
{
    class PreparedBufferWriter final
    {
        friend class FramePreparation;
        friend class InstanceLinkContext;
        explicit constexpr PreparedBufferWriter(std::uint32_t slot) noexcept : slot_(slot) {}
        std::uint32_t slot_;
    };

    class FrameBufferSizer final
    {
        friend class FramePreparation;
        friend class InstanceLinkContext;
        explicit constexpr FrameBufferSizer(std::uint32_t slot) noexcept : slot_(slot) {}
        std::uint32_t slot_;
    };

    class InstanceLinkContext;

    struct GraphCpuDataId final
    {
        std::uint32_t index = kGraphInvalidIndex;
        bool operator==(const GraphCpuDataId&) const noexcept = default;
    };

    struct GraphStorageSnapshotId final
    {
        std::uint32_t index = kGraphInvalidIndex;
        bool operator==(const GraphStorageSnapshotId&) const noexcept = default;
    };

    template <CpuPreparationValue T> class PreparedDataSlot final
    {
        friend class FramePreparation;
        friend class InstanceLinkContext;
        explicit constexpr PreparedDataSlot(std::uint32_t slot) noexcept : slot_(slot) {}
        std::uint32_t slot_;
    };

    template <ShaderConstantBlock T> class InlineConstantWriter final
    {
        friend class FramePreparation;
        friend class InstanceLinkContext;
        explicit constexpr InlineConstantWriter(std::uint32_t slot) noexcept : slot_(slot) {}
        std::uint32_t slot_;
    };

    class FramePreparation
    {
    public:
        virtual ~FramePreparation() noexcept = default;
        [[nodiscard]] virtual const TargetDesc& target() const noexcept = 0;
        [[nodiscard]] virtual RenderResult<void> setRasterStream(DrawStreamSlot, const RasterStreamData&) noexcept = 0;
        [[nodiscard]] virtual RenderResult<RasterProgramSlot>
        rasterProgram(DrawStreamSlot, std::uint32_t selection_index) const noexcept = 0;
        // Select a whole sampled image through the stream's qualified shared table.
        [[nodiscard]] virtual RenderResult<TextureGpuRef> sampleTexture(DrawStreamSlot, TextureId) noexcept = 0;
        // Geometry slices are valid only for this stream and the current prepare() call.
        [[nodiscard]] virtual RenderResult<RasterMeshGeometry> selectMesh(DrawStreamSlot, MeshId) noexcept = 0;
        // Select a record for the program that will read its page and textures in this preparation.
        [[nodiscard]] virtual RenderResult<rdesc::MaterialGpuReference>
        selectMaterial(DrawStreamSlot, RasterProgramSlot, MaterialId) noexcept = 0;

        // The Job owns this selection. The reference is valid only during prepare().
        template <class T>
        [[nodiscard]] RenderResult<PreparedStorageSnapshot<T>> readStorage(StorageSnapshotSlot<T> slot) const noexcept
        {
            auto selected = readStorageValue(slot.slot_, cxx::make_basic_type_info<T>());
            if (!selected) return cxx::unexpected(selected.error());
            return PreparedStorageSnapshot<T>{std::cref(*static_cast<const T*>(selected->value)), selected->revision};
        }

        template <CpuPreparationValue T>
        [[nodiscard]] RenderResult<PreparedCpuValue<T>> read(PreparedDataSlot<T> slot) const noexcept
        {
            auto selected = readBytes(slot.slot_, sizeof(T));
            if (!selected) return cxx::unexpected(selected.error());
            std::array<std::byte, sizeof(T)> bytes;
            std::memcpy(bytes.data(), selected->bytes.data(), bytes.size());
            return PreparedCpuValue<T>{std::bit_cast<T>(bytes), selected->revision};
        }

        template <CpuPreparationValue T>
        [[nodiscard]] RenderResult<void> publish(PreparedDataSlot<T> slot, const T& value) noexcept
        {
            return publishBytes(slot.slot_, std::as_bytes(std::span{&value, 1}));
        }

        // A nonzero revision identifies stable bytes from this writer's selected source.
        template <ShaderConstantBlock T>
        [[nodiscard]] RenderResult<void>
        write(InlineConstantWriter<T> writer, const T& value, std::uint64_t revision = 0) noexcept
        {
            return writeInlineBlock(writer.slot_, std::as_bytes(std::span{&value, 1}), revision);
        }

        // Publish a complete Buffer payload. A nonzero revision identifies unchanged bytes in later calls.
        [[nodiscard]] RenderResult<void> writeBuffer(
            PreparedBufferWriter writer, std::span<const std::byte> bytes, std::uint64_t revision = 0
        ) noexcept
        {
            return writeGpuBuffer(writer.slot_, bytes, revision);
        }

        // Set the logical range for the current frame. Graph commands define its contents.
        [[nodiscard]] RenderResult<void> sizeBuffer(FrameBufferSizer slot, std::uint64_t bytes) noexcept
        {
            return sizeGpuBuffer(slot.slot_, bytes);
        }

    protected:
        struct SelectedBytes final
        {
            std::span<const std::byte> bytes;
            std::uint64_t revision;
        };

    private:
        [[nodiscard]] virtual RenderResult<extension_detail::SelectedStorageSnapshot>
        readStorageValue(std::uint32_t, cxx::basic_type_info) const noexcept = 0;
        [[nodiscard]] virtual RenderResult<SelectedBytes> readBytes(std::uint32_t, std::size_t) const noexcept = 0;
        [[nodiscard]] virtual RenderResult<void> publishBytes(std::uint32_t, std::span<const std::byte>) noexcept = 0;
        [[nodiscard]] virtual RenderResult<void>
        writeInlineBlock(std::uint32_t, std::span<const std::byte>, std::uint64_t revision) noexcept = 0;
        [[nodiscard]] virtual RenderResult<void>
        writeGpuBuffer(std::uint32_t, std::span<const std::byte>, std::uint64_t) noexcept = 0;
        [[nodiscard]] virtual RenderResult<void> sizeGpuBuffer(std::uint32_t, std::uint64_t) noexcept = 0;
    };
} // namespace lux::render
