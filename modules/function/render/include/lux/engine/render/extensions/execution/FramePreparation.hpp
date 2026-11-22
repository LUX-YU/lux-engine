#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/execution/PreparedCpuValue.hpp>
#include <lux/engine/render/extensions/execution/StorageSnapshot.hpp>
#include <lux/engine/render/graph/DrawList.hpp>
#include <lux/engine/render/graph/DrawResource.hpp>
#include <lux/engine/render/graph/ConstantBlock.hpp>
#include <lux/engine/render/extensions/graph/GraphError.hpp>
#include <lux/engine/render/graph/IndexedGeometry.hpp>
#include <array>
#include <bit>
#include <cstring>
#include <functional>
#include <span>

namespace lux::rdesc { struct ShaderBlock; }

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

    template <CpuPreparationValue T> class CpuInputSlot final
    {
        friend class FramePreparation;
        friend class InstanceLinkContext;
        explicit constexpr CpuInputSlot(std::uint32_t slot) noexcept : slot_(slot) {}
        std::uint32_t slot_;
    };

    template <CpuPreparationValue T> class CpuOutputSlot final
    {
        friend class FramePreparation;
        friend class InstanceLinkContext;
        explicit constexpr CpuOutputSlot(std::uint32_t slot) noexcept : slot_(slot) {}
        std::uint32_t slot_;
    };

    template <ShaderConstantBlock T> class InlineConstantWriter final
    {
        friend class FramePreparation;
        friend class InstanceLinkContext;
        explicit constexpr InlineConstantWriter(std::uint32_t slot) noexcept : slot_(slot) {}
        std::uint32_t slot_;
    };

    class FramePreparation;

    struct RasterRecordLayout final
    {
        const rdesc::ShaderBlock& block;
        std::uint32_t frame_slot;
        bool reads_images;
    };

    namespace extension_detail
    {
        struct RasterRecordDestination final
        {
            DrawSourceAddress source;
            std::uint32_t program;
            RasterRecordLayout layout;
        };
    }

    // Borrows this preparation and its selected Program until prepare() returns.
    class RasterRecordWriter final
    {
    public:
        [[nodiscard]] const RasterRecordLayout& layout() const & noexcept { return destination_.layout; }
        const RasterRecordLayout& layout() const && = delete;

        [[nodiscard]] RenderResult<void> write(const RasterRecordInput& record) const noexcept;

        template <class Storage>
        [[nodiscard]] RenderResult<void>
        write(const RasterRecordInput& record, DrawResourceSlot<Storage> images) const noexcept;

    private:
        friend class FramePreparation;

        RasterRecordWriter(FramePreparation& frame, extension_detail::RasterRecordDestination destination) noexcept
            : frame_(frame), destination_(destination) {}

        FramePreparation& frame_;
        extension_detail::RasterRecordDestination destination_;
    };

    class FramePreparation
    {
    public:
        virtual ~FramePreparation() noexcept = default;
        [[nodiscard]] virtual const TargetDesc& target() const noexcept = 0;
        [[nodiscard]] virtual RenderResult<void> setRasterStream(DrawStreamSlot, const RasterStreamData&) noexcept = 0;
        [[nodiscard]] virtual RenderResult<RasterProgramSlot>
        rasterProgram(DrawStreamSlot, std::uint32_t selection_index) const noexcept = 0;
        template <class Storage>
        [[nodiscard]] RenderResult<RasterRecordWriter>
        rasterRecord(DrawResourceSlot<Storage> source, RasterProgramSlot program) noexcept
        {
            auto contract = recordLayout(source.address_, program);
            if (!contract) return cxx::unexpected(contract.error());
            return RasterRecordWriter{*this, {source.address_, program.index, *contract}};
        }

        // Borrow the installed Resource only during this preparation. An absent optional source returns null.
        template <class Storage>
        [[nodiscard]] RenderResult<const Storage*> readDrawResource(DrawResourceSlot<Storage> slot) const noexcept
        {
            const auto selected = readDrawSource(slot.address_, cxx::make_basic_type_info<Storage>());
            if (!selected) return cxx::unexpected(selected.error());
            return static_cast<const Storage*>(*selected);
        }

        template <class Storage>
        [[nodiscard]] RenderResult<RasterIndexedGeometry>
        bindIndexedGeometry(DrawResourceSlot<Storage> source, const IndexedGeometryInput& geometry) noexcept
        {
            return appendIndexedGeometry(source.address_, geometry);
        }

        template <class Storage>
        [[nodiscard]] RenderResult<void>
        useSampledImage(DrawResourceSlot<Storage> source, const nri::Texture& image, GraphImageRange range) noexcept
        {
            return appendSampledImage(source.address_, image, range);
        }

        // The Job owns this selection. The reference is valid only during prepare().
        template <class T>
        [[nodiscard]] RenderResult<PreparedStorageSnapshot<T>> readStorage(StorageSnapshotSlot<T> slot) const noexcept
        {
            auto selected = readStorageValue(slot.operand_);
            if (!selected) return cxx::unexpected(selected.error());
            return PreparedStorageSnapshot<T>{std::cref(*static_cast<const T*>(selected->value)), selected->revision};
        }

        template <CpuPreparationValue T>
        [[nodiscard]] RenderResult<PreparedCpuValue<T>> read(CpuInputSlot<T> slot) const noexcept
        {
            auto selected = readBytes(slot.slot_, sizeof(T));
            if (!selected) return cxx::unexpected(selected.error());
            std::array<std::byte, sizeof(T)> bytes;
            std::memcpy(bytes.data(), selected->bytes.data(), bytes.size());
            return PreparedCpuValue<T>{std::bit_cast<T>(bytes), selected->revision};
        }

        template <CpuPreparationValue T>
        [[nodiscard]] RenderResult<void> publish(CpuOutputSlot<T> slot, const T& value) noexcept
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
        friend class RasterRecordWriter;
        [[nodiscard]] virtual RenderResult<RasterRecordLayout>
        recordLayout(extension_detail::DrawSourceAddress, RasterProgramSlot) const noexcept = 0;
        [[nodiscard]] virtual RenderResult<void>
        appendRasterRecord(
            const extension_detail::RasterRecordDestination&,
            const RasterRecordInput&,
            std::optional<extension_detail::DrawSourceAddress>
        ) noexcept = 0;

        [[nodiscard]] virtual RenderResult<RasterIndexedGeometry>
        appendIndexedGeometry(extension_detail::DrawSourceAddress, const IndexedGeometryInput&) noexcept = 0;

        [[nodiscard]] virtual RenderResult<void>
        appendSampledImage(extension_detail::DrawSourceAddress, const nri::Texture&, GraphImageRange) noexcept = 0;
        [[nodiscard]] virtual RenderResult<const void*>
        readDrawSource(extension_detail::DrawSourceAddress, cxx::basic_type_info) const noexcept = 0;

        [[nodiscard]] virtual RenderResult<extension_detail::SelectedStorageSnapshot>
        readStorageValue(extension_detail::SnapshotOperand) const noexcept = 0;
        [[nodiscard]] virtual RenderResult<SelectedBytes> readBytes(std::uint32_t, std::size_t) const noexcept = 0;
        [[nodiscard]] virtual RenderResult<void> publishBytes(std::uint32_t, std::span<const std::byte>) noexcept = 0;
        [[nodiscard]] virtual RenderResult<void>
        writeInlineBlock(std::uint32_t, std::span<const std::byte>, std::uint64_t revision) noexcept = 0;
        [[nodiscard]] virtual RenderResult<void>
        writeGpuBuffer(std::uint32_t, std::span<const std::byte>, std::uint64_t) noexcept = 0;
        [[nodiscard]] virtual RenderResult<void> sizeGpuBuffer(std::uint32_t, std::uint64_t) noexcept = 0;
    };

    inline RenderResult<void> RasterRecordWriter::write(const RasterRecordInput& record) const noexcept
    {
        return frame_.appendRasterRecord(destination_, record, {});
    }

    template <class Storage>
    RenderResult<void>
    RasterRecordWriter::write(const RasterRecordInput& record, DrawResourceSlot<Storage> images) const noexcept
    {
        return frame_.appendRasterRecord(destination_, record, images.address_);
    }
} // namespace lux::render
